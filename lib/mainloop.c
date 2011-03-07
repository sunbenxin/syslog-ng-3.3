/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */
#include "mainloop.h"
#include "apphook.h"
#include "cfg.h"
#include "stats.h"
#include "messages.h"
#include "children.h"
#include "misc.h"
#include "control.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <iv.h>
#include <iv_signal.h>
#include <iv_work.h>

/**
 * Processing model
 * ================
 *
 * This comment documents how the work performed by syslog-ng is
 * partitioned between threads. First of all it is useful to know that
 * the configuration is translated to a tree of LogPipe instances, as
 * described in a comment in logpipe.h.
 *
 * The basic assumptions for threading:
 *   - configuration file is parsed in the main thread
 *   - the log pipe tree is built in the main thread
 *   - processing of messages is stalled while the
 *     configuration is reloaded
 *   - the _queue() operation for LogPipe instances can happen in
 *     multiple threads
 *   - notifications accross LogPipe instances happen in the main thread
 *
 * This boils down to:
 * ===================
 *   - If not otherwise specified LogPipe derived classes can only be
 *     instantiated (e.g. new()) or initialized/deinitialized (_init/deinit)
 *     in the main thread. Exceptions to this rule are documented below.
 *   - All queue operations finish before either deinit is called.
 *   - Locking is only needed between multiple invocations of _queue()
 *     in parallel threads, and any other main thread activity.
 *
 * Threading model
 * ===============
 *   - the main thread manages the configuration and polls for I/O
 *     using epoll
 *   - whenever an I/O event happens, work may be delegated to worker
 *     threads. Currently only the LogReader/LogWriter classes make use
 *     of worker threads, everything else remains in the main thread
 *     (internal messages, incoming connections, etc).
 *   - _all_ I/O polling must be registered in the main thread (update_watches
 *     and friends)
 *
 */

/* parsed command line arguments */
static gchar *cfgfilename = PATH_SYSLOG_NG_CONF;
static const gchar *persist_file = PATH_PERSIST_CONFIG;
static gchar *ctlfilename = PATH_CONTROL_SOCKET;
static gchar *preprocess_into = NULL;
gboolean syntax_only = FALSE;


/* signal handling */
static struct iv_signal sighup_poll;
static struct iv_signal sigterm_poll;
static struct iv_signal sigint_poll;
static struct iv_signal sigchild_poll;


/* current running configuration */
static GlobalConfig *current_configuration;


/************************************************************************************
 * Cross-thread function calls into the main loop
 ************************************************************************************/
typedef struct _MainLoopTaskCallSite
{
  struct list_head list;
  MainLoopTaskFunc func;
  gpointer user_data;
  gpointer result;
  gboolean pending;
  gboolean wait;
  GCond *cond;
  GStaticMutex lock;
} MainLoopTaskCallSite;

static GStaticMutex main_task_lock = G_STATIC_MUTEX_INIT;
static struct list_head main_task_queue = LIST_HEAD_INIT(main_task_queue);
static struct iv_event main_task_posted;
__thread gboolean in_main_thread;
static __thread MainLoopTaskCallSite call_info;

gpointer
main_loop_call(MainLoopTaskFunc func, gpointer user_data, gboolean wait)
{
  if (in_main_thread)
    return func(user_data);

  g_static_mutex_lock(&main_task_lock);

  /* check if a previous call is being executed */
  g_static_mutex_lock(&call_info.lock);
  if (call_info.pending)
    {
      /* yes, it is still running, indicate that we need to be woken up */
      call_info.wait = TRUE;
      g_static_mutex_unlock(&call_info.lock);

      while (call_info.pending)
        g_cond_wait(call_info.cond, g_static_mutex_get_mutex(&main_task_lock));
    }
  else
    {
      g_static_mutex_unlock(&call_info.lock);
    }

  /* call_info.lock is no longer needed, since we're the only ones using call_info now */
  INIT_LIST_HEAD(&call_info.list);
  call_info.pending = TRUE;
  call_info.func = func;
  call_info.user_data = user_data;
  call_info.wait = wait;
  if (!call_info.cond)
    call_info.cond = g_cond_new();
  list_add(&call_info.list, &main_task_queue);
  iv_event_post(&main_task_posted);
  if (wait)
    {
      while (call_info.pending)
        g_cond_wait(call_info.cond, g_static_mutex_get_mutex(&main_task_lock));
    }
  g_static_mutex_unlock(&main_task_lock);
  return call_info.result;
}

static void
main_loop_call_handler(gpointer user_data)
{
  g_static_mutex_lock(&main_task_lock);
  while (!list_empty(&main_task_queue))
    {
      MainLoopTaskCallSite *site;
      gpointer result;

      site = list_entry(main_task_queue.next, MainLoopTaskCallSite, list);
      list_del_init(&site->list);
      g_static_mutex_unlock(&main_task_lock);

      result = site->func(site->user_data);

      g_static_mutex_lock(&site->lock);
      site->result = result;
      site->pending = FALSE;
      g_static_mutex_unlock(&site->lock);

      g_static_mutex_lock(&main_task_lock);
      if (site->wait)
        g_cond_signal(site->cond);
    }
  g_static_mutex_unlock(&main_task_lock);
}

void
main_loop_call_init(void)
{
  IV_EVENT_INIT(&main_task_posted);
  main_task_posted.cookie = NULL;
  main_task_posted.handler = main_loop_call_handler;
  iv_event_register(&main_task_posted);
}

/************************************************************************************
 * stats timer
 ************************************************************************************/

/* stats timer */
static struct iv_timer stats_timer;
static gboolean stats_timer_first = TRUE;

void
stats_timer_elapsed(gpointer st)
{
  gint stats_freq = GPOINTER_TO_UINT(st);

  if (G_LIKELY(!stats_timer_first))
    stats_generate_log();

  iv_validate_now();
  stats_timer.expires = now;
  timespec_add_msec(&stats_timer.expires, stats_freq * 1000);
  iv_timer_register(&stats_timer);
  stats_timer_first = FALSE;
}


/************************************************************************************
 * I/O worker threads
 ************************************************************************************/

static struct iv_work_pool main_loop_io_workers;
static void (*main_loop_io_workers_sync_func)(void);

/* number of I/O worker jobs running */
static gint main_loop_io_workers_running;

/* cause workers to stop, no new I/O jobs to be submitted */
volatile gboolean main_loop_io_workers_quit;


void
main_loop_io_worker_job_submit(MainLoopIOWorkerJob *self)
{
  g_assert(self->working == FALSE);
  if (main_loop_io_workers_quit)
    return;
  main_loop_io_workers_running++;
  self->working = TRUE;
  iv_work_pool_submit_work(&main_loop_io_workers, &self->work_item);
}

void
main_loop_io_worker_job_start(MainLoopIOWorkerJob *self)
{
  self->work(self->user_data);
}

void
main_loop_io_worker_job_finish(MainLoopIOWorkerJob *self)
{
  self->completion(self->user_data);
  self->working = FALSE;
  main_loop_io_workers_running--;
  if (main_loop_io_workers_quit && main_loop_io_workers_running == 0)
    {
      main_loop_io_workers_quit = FALSE;
      main_loop_io_workers_sync_func();
      main_loop_io_workers_sync_func = NULL;
    }
}

void
main_loop_io_worker_job_init(MainLoopIOWorkerJob *self)
{
  IV_WORK_ITEM_INIT(&self->work_item);
  self->work_item.cookie = self;
  self->work_item.work = (void (*)(void *)) main_loop_io_worker_job_start;
  self->work_item.completion = (void (*)(void *)) main_loop_io_worker_job_finish;
}

void
main_loop_io_worker_sync_call(void (*func)(void))
{
  if (main_loop_io_workers_running == 0)
    {
      func();
    }
  else
    {
      main_loop_io_workers_quit = TRUE;
      main_loop_io_workers_sync_func = func;
    }
}

/************************************************************************************
 * config load/reload
 ************************************************************************************/

/* the old configuration that is being reloaded */
static GlobalConfig *main_loop_old_config;
/* the pending configuration we wish to switch to */
static GlobalConfig *main_loop_new_config;


/* called when syslog-ng first starts up */
static gboolean
main_loop_initialize_state(GlobalConfig *cfg, const gchar *persist_filename)
{
  gboolean success;

  cfg->state = persist_state_new(persist_filename);
  if (!persist_state_start(cfg->state))
    return FALSE;

  success = cfg_init(cfg);
  if (success)
    persist_state_commit(cfg->state);
  else
    persist_state_cancel(cfg->state);
  return success;
}

/* called to apply the new configuration once all I/O worker threads have finished */
static void
main_loop_reload_config_apply(void)
{
  main_loop_old_config->persist = persist_config_new();
  cfg_deinit(main_loop_old_config);
  cfg_persist_config_move(main_loop_old_config, main_loop_new_config);

  if (cfg_init(main_loop_new_config))
    {
      msg_verbose("New configuration initialized", NULL);
      persist_config_free(main_loop_new_config->persist);
      main_loop_new_config->persist = NULL;
      cfg_free(main_loop_old_config);
      current_configuration = main_loop_new_config;
    }
  else
    {
      msg_error("Error initializing new configuration, reverting to old config", NULL);
      cfg_persist_config_move(main_loop_new_config, main_loop_old_config);
      if (!cfg_init(main_loop_old_config))
        {
          /* hmm. hmmm, error reinitializing old configuration, we're hosed.
           * Best is to kill ourselves in the hope that the supervisor
           * restarts us.
           */
          kill(getpid(), SIGQUIT);
          g_assert_not_reached();
        }
      persist_config_free(main_loop_old_config->persist);
      main_loop_old_config->persist = NULL;
      cfg_free(main_loop_new_config);
      current_configuration = main_loop_old_config;
      goto finish;
    }

  /* this is already running with the new config in place */
  app_post_config_loaded();
  msg_notice("Configuration reload request received, reloading configuration",
               NULL);

 finish:
  main_loop_new_config = NULL;
  main_loop_old_config = NULL;

  reset_cached_hostname();

  /* stats: should probably be put to the stats.c module */
  if (iv_timer_registered(&stats_timer))
    iv_timer_unregister(&stats_timer);
  stats_timer.cookie = GINT_TO_POINTER(current_configuration->stats_freq);
  if (current_configuration->stats_freq > 0)
    {
      stats_timer_first = TRUE;
      stats_timer_elapsed(stats_timer.cookie);
    }
  stats_cleanup_orphans();
  return;
}

/* initiate configuration reload */
static void
main_loop_reload_config_initiate(void)
{
  main_loop_old_config = current_configuration;
  app_pre_config_loaded();
  main_loop_new_config = cfg_new(0);
  if (!cfg_read_config(main_loop_new_config, cfgfilename, FALSE, NULL))
    {
      cfg_free(main_loop_new_config);
      main_loop_new_config = NULL;
      main_loop_old_config = NULL;
      msg_error("Error parsing configuration",
                evt_tag_str(EVT_TAG_FILENAME, cfgfilename),
                NULL);
      return;
    }
  main_loop_io_worker_sync_call(main_loop_reload_config_apply);
}

/************************************************************************************
 * syncronized exit
 ************************************************************************************/

static struct iv_timer main_loop_exit_timer;

static void
main_loop_exit_finish(void)
{
  iv_quit();
}

static void
main_loop_exit_timer_elapsed(void *arg)
{
  main_loop_io_worker_sync_call(main_loop_exit_finish);
}



/************************************************************************************
 * signal handlers
 ************************************************************************************/

static void
sig_hup_handler(void *s)
{
  /* this may handle multiple SIGHUP signals, however it doesn't
   * really matter if we received only a single or multiple SIGHUPs
   * until we make sure that we handle the last one.  Since we
   * blocked the SIGHUP signal and reset sig_hup_received to FALSE,
   * we can be sure that if we receive an additional SIGHUP during
   * signal processing we get the new one when we finished this, and
   * handle that one as well. */

  main_loop_reload_config_initiate();
}

static void
sig_term_handler(void *s)
{
  if (main_loop_exit_timer.handler && iv_timer_registered(&main_loop_exit_timer))
    return;

  msg_notice("syslog-ng shutting down",
             evt_tag_str("version", VERSION),
             NULL);

  IV_TIMER_INIT(&main_loop_exit_timer);
  iv_validate_now();
  main_loop_exit_timer.expires = now;
  main_loop_exit_timer.handler = main_loop_exit_timer_elapsed;
  timespec_add_msec(&main_loop_exit_timer.expires, 100);
  iv_timer_register(&main_loop_exit_timer);
}

static void
sig_child_handler(void *s)
{
  pid_t pid;
  int status;

  /* this may handle multiple SIGCHLD signals, however it doesn't
   * matter if one or multiple SIGCHLD was received assuming that
   * all exited child process are waited for */

  do
    {
      pid = waitpid(-1, &status, WNOHANG);
      child_manager_sigchild(pid, status);
    }
  while (pid > 0);
}

static void
setup_signals(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);
}

/************************************************************************************
 * syslog-ng main loop
 ************************************************************************************/

/*
 * Returns: exit code to be returned to the calling process.
 */
int
main_loop_init(void)
{
  in_main_thread = TRUE;

  app_startup();
  setup_signals();
  iv_work_pool_create(&main_loop_io_workers);
  main_loop_call_init();

  current_configuration = cfg_new(0);
  if (!cfg_read_config(current_configuration, cfgfilename, syntax_only, preprocess_into))
    {
      return 1;
    }

  if (syntax_only || preprocess_into)
    {
      return 0;
    }

  if (!main_loop_initialize_state(current_configuration, persist_file))
    {
      return 2;
    }
  return 0;
}

int
main_loop_run(void)
{
  msg_notice("syslog-ng starting up",
             evt_tag_str("version", VERSION),
             NULL);

  IV_TIMER_INIT(&stats_timer);
  stats_timer.cookie = GINT_TO_POINTER(current_configuration->stats_freq);
  stats_timer.handler = stats_timer_elapsed;

  control_init(ctlfilename);

  IV_SIGNAL_INIT(&sighup_poll);
  sighup_poll.signum = SIGHUP;
  sighup_poll.exclusive = 1;
  sighup_poll.cookie = NULL;
  sighup_poll.handler = sig_hup_handler;
  iv_signal_register(&sighup_poll);

  IV_SIGNAL_INIT(&sigchild_poll);
  sigchild_poll.signum = SIGCHLD;
  sigchild_poll.exclusive = 1;
  sigchild_poll.handler = sig_child_handler;
  iv_signal_register(&sigchild_poll);

  IV_SIGNAL_INIT(&sigterm_poll);
  sigterm_poll.signum = SIGTERM;
  sigterm_poll.exclusive = 1;
  sigterm_poll.handler = sig_term_handler;
  iv_signal_register(&sigterm_poll);

  IV_SIGNAL_INIT(&sigint_poll);
  sigint_poll.signum = SIGINT;
  sigint_poll.exclusive = 1;
  sigint_poll.handler = sig_term_handler;
  iv_signal_register(&sigint_poll);

  /* NOTE: generate first stats message, which rearms the timer */
  stats_timer_first = TRUE;
  stats_timer_elapsed(stats_timer.cookie);

  /* main loop */
  iv_main();

  control_destroy();
  msg_notice("syslog-ng shutting down",
             evt_tag_str("version", VERSION),
             NULL);

  cfg_deinit(current_configuration);
  cfg_free(current_configuration);
  current_configuration = NULL;
  return 0;
}


static GOptionEntry main_loop_options[] =
{
  { "cfgfile",           'f',         0, G_OPTION_ARG_STRING, &cfgfilename, "Set config file name, default=" PATH_SYSLOG_NG_CONF, "<config>" },
  { "persist-file",      'R',         0, G_OPTION_ARG_STRING, &persist_file, "Set the name of the persistent configuration file, default=" PATH_PERSIST_CONFIG, "<fname>" },
  { "preprocess-into",     0,         0, G_OPTION_ARG_STRING, &preprocess_into, "Write the preprocessed configuration file to the file specified", "output" },
  { "worker-threads",      0,         0, G_OPTION_ARG_INT, &main_loop_io_workers.max_threads, "Set the number of I/O worker threads", "<max>" },
  { "syntax-only",       's',         0, G_OPTION_ARG_NONE, &syntax_only, "Only read and parse config file", NULL},
  { "control",           'c',         0, G_OPTION_ARG_STRING, &ctlfilename, "Set syslog-ng control socket, default=" PATH_CONTROL_SOCKET, "<ctlpath>" },
  { NULL },
};

void
main_loop_add_options(GOptionContext *ctx)
{
#ifdef _SC_NPROCESSORS_ONLN
  main_loop_io_workers.max_threads = 2 * sysconf(_SC_NPROCESSORS_ONLN);
#endif

  g_option_context_add_main_entries(ctx, main_loop_options, NULL);
}