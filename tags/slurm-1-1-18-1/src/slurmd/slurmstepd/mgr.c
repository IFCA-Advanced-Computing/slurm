/*****************************************************************************\
 *  src/slurmd/slurmstepd/mgr.c - job manager functions for slurmstepd
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission 
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and 
 *  distribute linked combinations including the two. You must obey the GNU 
 *  General Public License in all respects for all of the code used other than 
 *  OpenSSL. If you modify file(s) with this exception, you may extend this 
 *  exception to your version of the file(s), but you are not obligated to do 
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in 
 *  the program, then also delete it here.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#if HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif

#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#ifdef HAVE_AIX
#  undef HAVE_UNSETENV
#  include <sys/checkpnt.h>
#endif
#ifndef HAVE_UNSETENV
#  include "src/common/unsetenv.h"
#endif

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <time.h>

#if HAVE_STDLIB_H
#  include <stdlib.h>
#endif

#include <slurm/slurm_errno.h>

#include "src/common/cbuf.h"
#include "src/common/env.h"
#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/node_select.h"
#include "src/common/fd.h"
#include "src/common/safeopen.h"
#include "src/common/slurm_jobacct.h"
#include "src/common/switch.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"
#include "src/common/xmalloc.h"
#include "src/common/util-net.h"
#include "src/common/forward.h"
#include "src/common/plugstack.h"

#include "src/slurmd/slurmd/slurmd.h"

#include "src/slurmd/common/setproctitle.h"
#include "src/slurmd/common/proctrack.h"
#include "src/slurmd/common/task_plugin.h"
#include "src/slurmd/common/run_script.h"
#include "src/slurmd/common/reverse_tree.h"

#include "src/slurmd/slurmstepd/slurmstepd.h"
#include "src/slurmd/slurmstepd/mgr.h"
#include "src/slurmd/slurmstepd/task.h"
#include "src/slurmd/slurmstepd/io.h"
#include "src/slurmd/slurmstepd/pdebug.h"
#include "src/slurmd/slurmstepd/req.h"
#include "src/slurmd/slurmstepd/pam_ses.h"

#define RETRY_DELAY 15		/* retry every 15 seconds */
#define MAX_RETRY   240		/* retry 240 times (one hour max) */

/*
 *  List of signals to block in this process
 */
static int mgr_sigarray[] = {
	SIGINT,  SIGTERM, SIGTSTP,
	SIGQUIT, SIGPIPE, SIGUSR1,
	SIGUSR2, SIGALRM, SIGHUP, 0
};

struct priv_state {
	uid_t           saved_uid;
	gid_t           saved_gid;
	gid_t *         gid_list;
	int             ngids;
	char            saved_cwd [4096];
};

step_complete_t step_complete = {
	PTHREAD_COND_INITIALIZER,
	PTHREAD_MUTEX_INITIALIZER,
	-1,
	-1,
	-1,
	{},
	-1,
	-1,
	(bitstr_t *)NULL,
	0,
        NULL
};


/* 
 * Prototypes
 */

/* 
 * Job manager related prototypes
 */
static void _send_launch_failure(launch_tasks_request_msg_t *, 
                                 slurm_addr *, int);
static int  _fork_all_tasks(slurmd_job_t *job);
static int  _become_user(slurmd_job_t *job, struct priv_state *ps);
static void  _set_prio_process (slurmd_job_t *job);
static void _set_job_log_prefix(slurmd_job_t *job);
static int  _setup_io(slurmd_job_t *job);
static int  _setup_spawn_io(slurmd_job_t *job);
static int  _drop_privileges(slurmd_job_t *job, bool do_setuid,
				struct priv_state *state);
static int  _reclaim_privileges(struct priv_state *state);
static void _send_launch_resp(slurmd_job_t *job, int rc);
static void _slurmd_job_log_init(slurmd_job_t *job);
static void _wait_for_io(slurmd_job_t *job);
static int  _send_exit_msg(slurmd_job_t *job, uint32_t *tid, int n, 
		int status);
static int  _send_pending_exit_msgs(slurmd_job_t *job);
static void _kill_running_tasks(slurmd_job_t *job);
static void _wait_for_all_tasks(slurmd_job_t *job);
static int  _wait_for_any_task(slurmd_job_t *job, bool waitflag);

static void _setargs(slurmd_job_t *job);

static void _random_sleep(slurmd_job_t *job);
static char *_sprint_task_cnt(batch_job_launch_msg_t *msg);

/*
 * Batch job mangement prototypes:
 */
static char * _make_batch_dir(slurmd_job_t *job);
static char * _make_batch_script(batch_job_launch_msg_t *msg, char *path);
static int    _send_complete_batch_script_msg(slurmd_job_t *job, 
					      int err, int status);

/*
 * Initialize the group list using the list of gids from the slurmd if
 * available.  Otherwise initialize the groups with initgroups().
 */
static int _initgroups(slurmd_job_t *job);


static slurmd_job_t *reattach_job;

/*
 * Launch an job step on the current node
 */
extern slurmd_job_t *
mgr_launch_tasks_setup(launch_tasks_request_msg_t *msg, slurm_addr *cli,
		       slurm_addr *self)
{
	slurmd_job_t *job = NULL;

	if (!(job = job_create(msg, cli))) {
		_send_launch_failure (msg, cli, errno);
		return NULL;
	}

	_set_job_log_prefix(job);

	_setargs(job);
	
	job->envtp->cli = cli;
	job->envtp->self = self;
	
	return job;
}

static void
_batch_finish(slurmd_job_t *job, int rc)
{
	int status = job->task[0]->estatus;

	if (job->argv[0] && (unlink(job->argv[0]) < 0))
		error("unlink(%s): %m", job->argv[0]);
	if (job->batchdir && (rmdir(job->batchdir) < 0))
		error("rmdir(%s): %m",  job->batchdir);
	xfree(job->batchdir);
	if (job->stepid == NO_VAL) {
		verbose("job %u completed with slurm_rc = %d, job_rc = %d",
			job->jobid, rc, status);
	} else {
		verbose("job %u.%u completed with slurm_rc = %d, job_rc = %d",
			job->jobid, job->stepid, rc, status);
	}

	_send_complete_batch_script_msg(job, rc, status);
}

/*
 * Launch a batch job script on the current node
 */
slurmd_job_t *
mgr_launch_batch_job_setup(batch_job_launch_msg_t *msg, slurm_addr *cli)
{
	slurmd_job_t *job = NULL;
	char       buf[1024];
	hostlist_t hl = hostlist_create(msg->nodes);
	if (!hl)
		return NULL;
		
	hostlist_ranged_string(hl, 1024, buf);
	
	if (!(job = job_batch_job_create(msg))) {
		error("job_batch_job_create() failed: %m");
		return NULL;
	}

	_set_job_log_prefix(job);

	_setargs(job);

	if ((job->batchdir = _make_batch_dir(job)) == NULL) {
		goto cleanup1;
	}

	xfree(job->argv[0]);

	if ((job->argv[0] = _make_batch_script(msg, job->batchdir)) == NULL) {
		goto cleanup2;
	}
	
	job->envtp->nprocs = msg->nprocs;
	job->envtp->select_jobinfo = msg->select_jobinfo;
	job->envtp->nhosts = hostlist_count(hl);
	hostlist_destroy(hl);
	job->envtp->nodelist = xstrdup(buf);
	job->envtp->task_count = _sprint_task_cnt(msg);
	return job;

cleanup2:
	if (job->batchdir && (rmdir(job->batchdir) < 0))
		error("rmdir(%s): %m",  job->batchdir);
	xfree(job->batchdir);

cleanup1:
	error("batch script setup failed for job %u.%u",
	      msg->job_id, msg->step_id);

	_send_complete_batch_script_msg(job, -1, -1);

	return NULL;
}

/*
 * Spawn a task / job step on the current node
 */
slurmd_job_t *
mgr_spawn_task_setup(spawn_task_request_msg_t *msg, slurm_addr *cli,
		     slurm_addr *self)
{
	slurmd_job_t *job = NULL;
	
	if (!(job = job_spawn_create(msg, cli)))
		return NULL;

	job->spawn_task = true;
	_set_job_log_prefix(job);

	_setargs(job);
	
	job->envtp->cli = cli;
	job->envtp->self = self;
	
	return job;
}

static void
_set_job_log_prefix(slurmd_job_t *job)
{
	char buf[256];

	if (job->jobid > MAX_NOALLOC_JOBID) 
		return;

	if ((job->jobid >= MIN_NOALLOC_JOBID) || (job->stepid == NO_VAL)) 
		snprintf(buf, sizeof(buf), "[%u]", job->jobid);
	else
		snprintf(buf, sizeof(buf), "[%u.%u]", job->jobid, job->stepid);

	log_set_fpfx(buf);
}

static int
_setup_io(slurmd_job_t *job)
{
	int            rc   = 0;
	struct priv_state sprivs;

	debug2("Entering _setup_io");


	/*
	 * Temporarily drop permissions, initialize task stdio file
	 * decriptors (which may be connected to files), then
	 * reclaim privileges.
	 */
	if (_drop_privileges(job, true, &sprivs) < 0)
		return ESLURMD_SET_UID_OR_GID_ERROR;

	/* FIXME - need to check a return code for failures */
	io_init_tasks_stdio(job);

	if (_reclaim_privileges(&sprivs) < 0)
		error("sete{u/g}id(%lu/%lu): %m", 
		      (u_long) sprivs.saved_uid, (u_long) sprivs.saved_gid);

	/*
	 * MUST create the initial client object before starting
	 * the IO thread, or we risk losing stdout/err traffic.
	 */
	if (!job->batch) {
		srun_info_t *srun = list_peek(job->sruns);
		xassert(srun != NULL);
		rc = io_initial_client_connect(srun, job);
		if (rc < 0) 
			return ESLURMD_IO_ERROR;
	}

	if (!job->batch)
		if (io_thread_start(job) < 0)
			return ESLURMD_IO_ERROR;
	/*
	 * Initialize log facility to copy errors back to srun
	 */
	_slurmd_job_log_init(job);
	
#ifndef NDEBUG
#  ifdef PR_SET_DUMPABLE
	if (prctl(PR_SET_DUMPABLE, 1) < 0)
		debug ("Unable to set dumpable to 1");
#  endif /* PR_SET_DUMPABLE */
#endif   /* !NDEBUG         */

	debug2("Leaving  _setup_io");
	return SLURM_SUCCESS;
}


static int
_setup_spawn_io(slurmd_job_t *job)
{
	_slurmd_job_log_init(job);

#ifndef NDEBUG
#  ifdef PR_SET_DUMPABLE
	if (prctl(PR_SET_DUMPABLE, 1) < 0)
		debug ("Unable to set dumpable to 1");
#  endif /* PR_SET_DUMPABLE */
#endif   /* !NDEBUG         */

	return SLURM_SUCCESS;
}


static void
_random_sleep(slurmd_job_t *job)
{
	long int delay = 0;
	long int max   = (3 * job->nnodes); 

	srand48((long int) (job->jobid + job->nodeid));

	delay = lrand48() % ( max + 1 );
	debug3("delaying %dms", delay);
	if (poll(NULL, 0, delay) == -1)
		return;
}

/*
 * Send task exit message for n tasks. tid is the list of _global_
 * task ids that have exited
 */
static int
_send_exit_msg(slurmd_job_t *job, uint32_t *tid, int n, int status)
{
	slurm_msg_t     resp;
	task_exit_msg_t msg;
	ListIterator    i       = NULL;
	srun_info_t    *srun    = NULL;

	debug3("sending task exit msg for %d tasks", n);

	msg.task_id_list = tid;
	msg.num_tasks    = n;
	msg.return_code  = status;
	resp.data        = &msg;
	resp.msg_type    = MESSAGE_TASK_EXIT;
	resp.srun_node_id = job->nodeid;
	resp.forward.cnt = 0;
	resp.ret_list = NULL;
	resp.orig_addr.sin_addr.s_addr = 0;

	/*
	 *  XXX Hack for TCP timeouts on exit of large, synchronized
	 *  jobs. Delay a random amount if job->nnodes > 100
	 */
	if (job->nnodes > 100) 
		_random_sleep(job);

	/*
	 * XXX: Should srun_list be associated with each task?
	 */
	i = list_iterator_create(job->sruns);
	while ((srun = list_next(i))) {
		resp.address = srun->resp_addr;
		if (resp.address.sin_family != 0)
			slurm_send_only_node_msg(&resp);
	}
	list_iterator_destroy(i);

	return SLURM_SUCCESS;
}

static void
_wait_for_children_slurmstepd(slurmd_job_t *job)
{
	int left = 0;
	int rc;
	int i;
	struct timespec ts = {0, 0};

	pthread_mutex_lock(&step_complete.lock);

	/* wait an extra 3 seconds for every level of tree below this level */
	if (step_complete.children > 0) {
		ts.tv_sec += 3 * (step_complete.max_depth-step_complete.depth);
		ts.tv_sec += time(NULL) + REVERSE_TREE_CHILDREN_TIMEOUT;

		while((left = bit_clear_count(step_complete.bits)) > 0) {
			debug3("Rank %d waiting for %d (of %d) children",
			     step_complete.rank, left, step_complete.children);
			rc = pthread_cond_timedwait(&step_complete.cond,
						    &step_complete.lock, &ts);
			if (rc == ETIMEDOUT) {
				debug2("Rank %d timed out waiting for %d"
				       " (of %d) children", step_complete.rank,
				       left, step_complete.children);
				break;
			}
		}
		if (left == 0) {
			debug2("Rank %d got all children completions",
			       step_complete.rank);
		}
	} else {
		debug2("Rank %d has no children slurmstepd",
		       step_complete.rank);
	}

	/* Find the maximum task return code */
	for (i = 0; i < job->ntasks; i++)
		step_complete.step_rc = MAX(step_complete.step_rc,
					 WEXITSTATUS(job->task[i]->estatus));

	pthread_mutex_unlock(&step_complete.lock);
}


/*
 * Send a single step completion message, which represents a single range
 * of complete job step nodes.
 */
/* caller is holding step_complete.lock */
static void
_one_step_complete_msg(slurmd_job_t *job, int first, int last)
{
	slurm_msg_t req;
	step_complete_msg_t msg;
	int rc = -1;
	int retcode;
	int i;

	msg.job_id = job->jobid;
	msg.job_step_id = job->stepid;
	msg.range_first = first;
	msg.range_last = last;
	msg.step_rc = step_complete.step_rc;
	msg.jobacct = jobacct_g_alloc(NULL);
	/************* acct stuff ********************/
	jobacct_g_aggregate(step_complete.jobacct, job->jobacct);
	jobacct_g_getinfo(step_complete.jobacct, JOBACCT_DATA_TOTAL, 
			  msg.jobacct);
	/*********************************************/	
	memset(&req, 0, sizeof(req));
	req.msg_type = REQUEST_STEP_COMPLETE;
	req.data = &msg;
	req.address = step_complete.parent_addr;

	/* Do NOT change this check to "step_complete.rank == 0", because
	 * there are odd situations where SlurmUser or root could
	 * craft a launch without a valid credential, and no tree information
	 * can be built with out the hostlist from the credential.
	 */
	if (step_complete.parent_rank == -1) {
		/* this is the base of the tree, its parent is slurmctld */
		debug3("Rank %d sending complete to slurmctld, range %d to %d",
		       step_complete.rank, first, last);
		if (slurm_send_recv_controller_rc_msg(&req, &rc) < 0)
			error("Rank %d failed sending step completion message"
			      " to slurmctld (parent)", step_complete.rank);
		goto finished;
	}

	debug3("Rank %d sending complete to rank %d, range %d to %d",
	       step_complete.rank, step_complete.parent_rank, first, last);
	/* On error, pause then try sending to parent again.
	 * The parent slurmstepd may just not have started yet, because
	 * of the way that the launch message forwarding works.
	 */
	for (i = 0; i < REVERSE_TREE_PARENT_RETRY; i++) {
		if (i)
			sleep(1);
		retcode = slurm_send_recv_rc_msg_only_one(&req, &rc, 10000);
		if (retcode == 0 && rc == 0)
			goto finished;
	}
	/* on error AGAIN, send to the slurmctld instead */
	debug3("Rank %d sending complete to slurmctld instead, range %d to %d",
	       step_complete.rank, first, last);
	if (slurm_send_recv_controller_rc_msg(&req, &rc) < 0)
		error("Rank %d failed sending step completion message"
		      " directly to slurmctld", step_complete.rank);
finished:
	jobacct_g_free(msg.jobacct);
}

/* Given a starting bit in the step_complete.bits bitstring, "start",
 * find the next contiguous range of set bits and return the first
 * and last indices of the range in "first" and "last".
 *
 * caller is holding step_complete.lock
 */
static int
_bit_getrange(int start, int size, int *first, int *last)
{
	int i;
	bool found_first = false;

	for (i = start; i < size; i++) {
		if (bit_test(step_complete.bits, i)) {
			if (found_first) {
				*last = i;
				continue;
			} else {
				found_first = true;
				*first = i;
				*last = i;
			}
		} else {
			if (!found_first) {
				continue;
			} else {
				*last = i - 1;
				break;
			}
		}
	}

	if (found_first)
		return 1;
	else
		return 0;
}

/*
 * Send as many step completion messages as necessary to represent
 * all completed nodes in the job step.  There may be nodes that have
 * not yet signalled their completion, so there will be gaps in the
 * completed node bitmap, requiring that more than one message be sent.
 */
static void
_send_step_complete_msgs(slurmd_job_t *job)
{
	int start, size;
	int first=-1, last=-1;
	bool sent_own_comp_msg = false;

	pthread_mutex_lock(&step_complete.lock);
	start = 0;
	size = bit_size(step_complete.bits);

	/* If no children, send message and return early */
	if (size == 0) {
		_one_step_complete_msg(job, step_complete.rank,
				       step_complete.rank);
		pthread_mutex_unlock(&step_complete.lock);
		return;
	}

	while(_bit_getrange(start, size, &first, &last)) {
		/* THIS node is not in the bit string, so we need to prepend
		   the local rank */
		if (start == 0 && first == 0) {
			sent_own_comp_msg = true;
			first = -1;
		}

		_one_step_complete_msg(job, (first + step_complete.rank + 1),
	      			       (last + step_complete.rank + 1));
		start = last + 1;
	}

	if (!sent_own_comp_msg)
		_one_step_complete_msg(job, step_complete.rank,
				       step_complete.rank);

	pthread_mutex_unlock(&step_complete.lock);
}

/* 
 * Executes the functions of the slurmd job manager process,
 * which runs as root and performs shared memory and interconnect
 * initialization, etc.
 *
 * Returns 0 if job ran and completed successfully.
 * Returns errno if job startup failed.
 *
 */
int 
job_manager(slurmd_job_t *job)
{
	int  rc = 0;
	bool io_initialized = false;

	debug3("Entered job_manager for %u.%u pid=%lu",
	       job->jobid, job->stepid, (unsigned long) job->jmgr_pid);
	
	if (!job->batch &&
	    (interconnect_preinit(job->switch_job) < 0)) {
		rc = ESLURM_INTERCONNECT_FAILURE;
		goto fail1;
	}
	
	if (job->spawn_task)
		rc = _setup_spawn_io(job);
	else
		rc = _setup_io(job);
	if (rc) {
		error("IO setup failed: %m");
		goto fail2;
	} else {
		io_initialized = true;
	}
		
	/* Call interconnect_init() before becoming user */
	if (!job->batch && 
	    (interconnect_init(job->switch_job, job->uid) < 0)) {
		/* error("interconnect_init: %m"); already logged */
		rc = ESLURM_INTERCONNECT_FAILURE;
		io_close_task_fds(job);
		goto fail2;
	}

	/* calls pam_setup() and requires pam_finish() if successful */
	if (_fork_all_tasks(job) < 0) {
		debug("_fork_all_tasks failed");
		rc = ESLURMD_EXECVE_FAILED;
		io_close_task_fds(job);
		goto fail2;
	}

	io_close_task_fds(job);

	xsignal_block(mgr_sigarray);
	reattach_job = job;

	job->state = SLURMSTEPD_STEP_RUNNING;

	/* Send job launch response with list of pids */
	_send_launch_resp(job, 0);

	_wait_for_all_tasks(job);
	jobacct_g_endpoll();
		
	job->state = SLURMSTEPD_STEP_ENDING;

	/* 
	 * This just cleans up all of the PAM state and errors are logged
	 * below, so there's no need for error handling.
	 */
	pam_finish();

	if (!job->batch && 
	    (interconnect_fini(job->switch_job) < 0)) {
		error("interconnect_fini: %m");
	}

    fail2:
	/*
	 *  First call interconnect_postfini() - In at least one case,
	 *    this will clean up any straggling processes. If this call
	 *    is moved behind wait_for_io(), we may block waiting for IO
	 *    on a hung process.
	 */
	if (!job->batch) {
		_kill_running_tasks(job);
		if (interconnect_postfini(job->switch_job, job->jmgr_pid,
				job->jobid, job->stepid) < 0)
			error("interconnect_postfini: %m");
	}

	/*
	 * Wait for io thread to complete (if there is one)
	 */
	if (!job->batch && !job->spawn_task && io_initialized) {
		eio_signal_shutdown(job->eio);
		_wait_for_io(job);
	}

	if (spank_fini (job)  < 0) {
		error ("spank_fini failed\n");
	}

    fail1:
	/* If interactive job startup was abnormal, 
	 * be sure to notify client.
	 */
	if (rc != 0) {
		error("job_manager exiting abnormally, rc = %d", rc);
		_send_launch_resp(job, rc);
	}

	if (job->batch) {
		_batch_finish(job, rc); /* sends batch complete message */
	} else if (step_complete.rank > -1) {
		_wait_for_children_slurmstepd(job);
		_send_step_complete_msgs(job);
	}

	return(rc);
}


/* fork and exec N tasks
 */ 
static int
_fork_all_tasks(slurmd_job_t *job)
{
	int rc = SLURM_SUCCESS;
	int i;
	int *writefds; /* array of write file descriptors */
	int *readfds; /* array of read file descriptors */
	int fdpair[2];
	uint16_t propagate_prio = slurm_get_propagate_prio_process();
	struct priv_state sprivs;
	jobacct_id_t jobacct_id;

	xassert(job != NULL);

	if (slurm_container_create(job) == SLURM_ERROR) {
		error("slurm_container_create: %m");
		return SLURM_ERROR;
	}

	if (spank_init (job) < 0) {
		error ("Plugin stack initialization failed.\n");
		return SLURM_ERROR;
	}

	/*
	 * Pre-allocate a pipe for each of the tasks
	 */
	debug3("num tasks on this node = %d", job->ntasks);
	writefds = (int *) xmalloc (job->ntasks * sizeof(int));
	if (!writefds) {
		error("writefds xmalloc failed!");
		return SLURM_ERROR;
	}
	readfds = (int *) xmalloc (job->ntasks * sizeof(int));
	if (!readfds) {
		error("readfds xmalloc failed!");
		return SLURM_ERROR;
	}


	for (i = 0; i < job->ntasks; i++) {
		fdpair[0] = -1; fdpair[1] = -1;
		if (pipe (fdpair) < 0) {
			error ("exec_all_tasks: pipe: %m");
			return SLURM_ERROR;
		}
		debug3("New fdpair[0] = %d, fdpair[1] = %d", 
		       fdpair[0], fdpair[1]);
		fd_set_close_on_exec(fdpair[0]);
		fd_set_close_on_exec(fdpair[1]);
		readfds[i] = fdpair[0];
		writefds[i] = fdpair[1];
	}

	/* Temporarily drop effective privileges, except for the euid.
	 * We need to wait until after pam_setup() to drop euid.
	 */
	if (_drop_privileges (job, false, &sprivs) < 0)
		return ESLURMD_SET_UID_OR_GID_ERROR;

	if (pam_setup(job->pwd->pw_name, conf->hostname)
	    != SLURM_SUCCESS){
		error ("error in pam_setup");
		goto fail1;
	}

	if (seteuid (job->pwd->pw_uid) < 0) {
		error ("seteuid: %m");
		goto fail2;
	}

	if (chdir(job->cwd) < 0) {
		error("couldn't chdir to `%s': %m: going to /tmp instead",
		      job->cwd);
		if (chdir("/tmp") < 0) {
			error("couldn't chdir to /tmp either. dying.");
			goto fail2;
		}
	}

	if (spank_user (job) < 0) {
		error("spank_user failed.");
		return SLURM_ERROR;
	}

	/*
	 * Fork all of the task processes.
	 */
	for (i = 0; i < job->ntasks; i++) {
		pid_t pid;

		if ((pid = fork ()) < 0) {
			error("fork: %m");
			goto fail2;
		} else if (pid == 0)  { /* child */
			int j;

#ifdef HAVE_AIX
			(void) mkcrid(0);
#endif
			/* Close file descriptors not needed by the child */
			for (j = 0; j < job->ntasks; j++) {
				close(writefds[j]);
				if (j > i)
					close(readfds[j]);
			}

			if (propagate_prio == 1)
				_set_prio_process(job);

 			if (_become_user(job, &sprivs) < 0) {
 				error("_become_user failed: %m");
				/* child process, should not return */
				exit(1);
 			}

			/* log_fini(); */ /* note: moved into exec_task() */

			xsignal_unblock(slurmstepd_blocked_signals);

			exec_task(job, i, readfds[i]);
		}

		/*
		 * Parent continues: 
		 */

		close(readfds[i]);
		verbose ("task %lu (%lu) started %M", 
			(unsigned long) job->task[i]->gtid, 
			(unsigned long) pid); 

		job->task[i]->pid = pid;
		if (i == 0)
			job->pgid = pid;
	}

	/*
	 * All tasks are now forked and running as the user, but
	 * will wait for our signal before calling exec.
	 */

	/*
	 * Reclaim privileges
	 */
	if (_reclaim_privileges (&sprivs) < 0) {
		error ("Unable to reclaim privileges");
		/* Don't bother erroring out here */
	}

	if (chdir (sprivs.saved_cwd) < 0) {
		error ("Unable to return to working directory");
	}

	for (i = 0; i < job->ntasks; i++) {
		/*
                 * Put this task in the step process group
                 */
                if (setpgid (job->task[i]->pid, job->pgid) < 0)
                        error ("Unable to put task %d (pid %ld) into pgrp %ld",
                               i, job->task[i]->pid, job->pgid);

                if (slurm_container_add(job, job->task[i]->pid) == SLURM_ERROR) {
                        error("slurm_container_create: %m");
			goto fail1;
                }
		jobacct_id.nodeid = job->nodeid;
		jobacct_id.taskid = job->task[i]->gtid;
		jobacct_g_add_task(job->task[i]->pid, 
				   &jobacct_id);

		if (spank_task_post_fork (job, i) < 0) {
			error ("spank task %d post-fork failed", i);
			return SLURM_ERROR;
		}
	}

	/*
	 * Now it's ok to unblock the tasks, so they may call exec.
	 */
	for (i = 0; i < job->ntasks; i++) {
		char c = '\0';
		
		debug3("Unblocking %u.%u task %d, writefd = %d",
		       job->jobid, job->stepid, i, writefds[i]);
		if (write (writefds[i], &c, sizeof (c)) != 1)
			error ("write to unblock task %d failed", i); 

		close(writefds[i]);

		/*
		 * Prepare process for attach by parallel debugger 
		 * (if specified and able)
		 */
		if (pdebug_trace_process(job, job->task[i]->pid)
				== SLURM_ERROR)
			rc = SLURM_ERROR;
	}
	xfree(writefds);
	xfree(readfds);

	return rc;

fail2:
	_reclaim_privileges (&sprivs);
fail1:
	pam_finish();
	return SLURM_ERROR;
}


/*
 * Loop once through tasks looking for all tasks that have exited with
 * the same exit status (and whose statuses have not been sent back to
 * the client) Aggregate these tasks into a single task exit message.
 *
 */ 
static int 
_send_pending_exit_msgs(slurmd_job_t *job)
{
	int  i;
	int  nsent  = 0;
	int  status = 0;
	bool set    = false;
	uint32_t  tid[job->ntasks];

	/* 
	 * Collect all exit codes with the same status into a 
	 * single message. 
	 */
	for (i = 0; i < job->ntasks; i++) {
		slurmd_task_info_t *t = job->task[i];

		if (!t->exited || t->esent)
			continue;

		if (!set) { 
			status = t->estatus;
			set    = true;
		} else if (status != t->estatus)
			continue;

		tid[nsent++] = t->gtid;
		t->esent = true;
	}

	if (nsent) {
		debug2("Aggregated %d task exit messages", nsent);
		_send_exit_msg(job, tid, nsent, status);
	}

	return nsent;
}

/*
 * If waitflag is true, perform a blocking wait for a single process
 * and then return.
 *
 * If waitflag is false, do repeated non-blocking waits until
 * there are no more processes to reap (waitpid returns 0).
 *
 * Returns the number of tasks for which a wait3() was succesfully
 * performed, or -1 if there are no child tasks.
 */
static int
_wait_for_any_task(slurmd_job_t *job, bool waitflag)
{
	slurmd_task_info_t *t = NULL;
	int i;
	int status;
	pid_t pid;
	int completed = 0;
	jobacctinfo_t *jobacct = NULL;
	struct rusage rusage;
	do {
		pid = wait3(&status, waitflag ? 0 : WNOHANG, &rusage);
		if (pid == -1) {
			if (errno == ECHILD) {
				debug("No child processes");
				if (completed == 0)
					completed = -1;
				goto done;
			} else if (errno == EINTR) {
				debug("wait3 was interrupted");
				continue;
			} else {
				debug("Unknown errno %d", errno);
				continue;
			}
		} else if (pid == 0) { /* WNOHANG and no pids available */
			goto done;
		}

		/************* acct stuff ********************/
		jobacct = jobacct_g_remove_task(pid);
		if(jobacct) {
			jobacct_g_setinfo(jobacct, 
					  JOBACCT_DATA_RUSAGE, &rusage);
			jobacct_g_aggregate(job->jobacct, jobacct);
			jobacct_g_free(jobacct);
		} 		
		/*********************************************/	
	
		/* See if the pid matches that of one of the tasks */
		for (i = 0; i < job->ntasks; i++) {
			if (job->task[i]->pid == pid) {
				t = job->task[i];
				completed++;
				break;
			}
		}
		if (t != NULL) {
			verbose("task %lu (%lu) exited status 0x%04x %M",
				(unsigned long)job->task[i]->gtid,
				(unsigned long)pid, status);
			t->exited  = true;
			t->estatus = status;
			job->envtp->env = job->env;
			job->envtp->procid = job->task[i]->gtid;
			job->envtp->localid = job->task[i]->id;
			
			/* need to take this out in 1.2 */
			job->envtp->distribution = SLURM_DIST_UNKNOWN;
			setup_env(job->envtp);
			job->env = job->envtp->env;
			if (job->task_epilog) {
				run_script("user task_epilog", 
					   job->task_epilog, 
					   job->jobid, job->uid, 
					   2, job->env);
			}
			if (conf->task_epilog) {
				char *my_epilog;
				slurm_mutex_lock(&conf->config_mutex);
				my_epilog = xstrdup(conf->task_epilog);
				slurm_mutex_unlock(&conf->config_mutex);
				run_script("slurm task_epilog", my_epilog, 
					job->jobid, job->uid, -1, job->env);
				xfree(my_epilog);
			}
			job->envtp->procid = i;

			if (spank_task_exit (job, i) < 0)
				error ("Unable to spank task %d at exit", i);

			post_term(job);
		}

	} while ((pid > 0) && !waitflag);

done:
	return completed;
}
	

static void
_wait_for_all_tasks(slurmd_job_t *job)
{
	int tasks_left = 0;
	int i;

	for (i = 0; i < job->ntasks; i++) {
		if (job->task[i]->state < SLURMD_TASK_COMPLETE) {
			tasks_left++;
		}
	}
	if (tasks_left < job->ntasks)
		verbose("Only %d of %d requested tasks successfully launched",
			tasks_left, job->ntasks);

	for (i = 0; i < tasks_left; ) {
		int rc;
		rc = _wait_for_any_task(job, true);
		if (rc != -1) {
			i += rc;
			if (i < job->ntasks) {
				rc = _wait_for_any_task(job, false);
				if (rc != -1) {
					i += rc;
				}
			}
		}

		while (_send_pending_exit_msgs(job)) {;}
	}
}

/*
 * Make sure all processes in session are dead for interactive jobs.  On 
 * systems with an IBM Federation switch, all processes must be terminated 
 * before the switch window can be released by interconnect_postfini().
 *  For batch jobs, we let spawned processes continue by convention
 * (although this could go either way). The Epilog program could be used 
 * to terminate any "orphan" processes.
 */
static void
_kill_running_tasks(slurmd_job_t *job)
{
	int          delay = 1;

	if (job->batch)
		return;

	if (job->cont_id) {
		slurm_container_signal(job->cont_id, SIGKILL);

		/* Spin until the container is successfully destroyed */
		while (slurm_container_destroy(job->cont_id) != SLURM_SUCCESS) {
			slurm_container_signal(job->cont_id, SIGKILL);
			sleep(delay);
			if (delay < 120) {
				delay *= 2;
			} else {
				error("Unable to destroy container, job %u.%u",
				      job->jobid, job->stepid);
			}
		}
	}

	return;
}

/*
 * Wait for IO
 */
static void
_wait_for_io(slurmd_job_t *job)
{
	debug("Waiting for IO");
	io_close_all(job);

	/*
	 * Wait until IO thread exits
	 */
	if (job->ioid)
		pthread_join(job->ioid, NULL);
	else
		info("_wait_for_io: ioid==0");

	return;
}

	
static char *
_make_batch_dir(slurmd_job_t *job)
{
	char path[MAXPATHLEN]; 

	if (job->stepid == NO_VAL)
		snprintf(path, 1024, "%s/job%05u", conf->spooldir, job->jobid);
	else
		snprintf(path, 1024, "%s/job%05u.%05u", conf->spooldir, job->jobid,
			job->stepid);

	if ((mkdir(path, 0750) < 0) && (errno != EEXIST)) {
		error("mkdir(%s): %m", path);
		goto error;
	}

	if (chown(path, (uid_t) -1, (gid_t) job->pwd->pw_gid) < 0) {
		error("chown(%s): %m", path);
		goto error;
	}

	if (chmod(path, 0750) < 0) {
		error("chmod(%s, 750): %m");
		goto error;
	}

	return xstrdup(path);

   error:
	return NULL;
}

static char *
_make_batch_script(batch_job_launch_msg_t *msg, char *path)
{
	FILE *fp = NULL;
	char  script[MAXPATHLEN];

	snprintf(script, 1024, "%s/%s", path, "script"); 

  again:
	if ((fp = safeopen(script, "w", SAFEOPEN_CREATE_ONLY)) == NULL) {
		if ((errno != EEXIST) || (unlink(script) < 0))  {
			error("couldn't open `%s': %m", script);
			goto error;
		}
		goto again;
	}

	if (fputs(msg->script, fp) < 0) {
		error("fputs: %m");
		goto error;
	}

	if (fclose(fp) < 0) {
		error("fclose: %m");
	}
	
	if (chown(script, (uid_t) msg->uid, (gid_t) -1) < 0) {
		error("chown(%s): %m", path);
		goto error;
	}

	if (chmod(script, 0500) < 0) {
		error("chmod: %m");
	}

	return xstrdup(script);

  error:
	return NULL;

}

static char *
_sprint_task_cnt(batch_job_launch_msg_t *msg)
{
        int i;
        char *task_str = xstrdup("");
        char tmp[16], *comma = "";
	for (i=0; i<msg->num_cpu_groups; i++) {
		if (i == 1)
			comma = ",";
		if (msg->cpu_count_reps[i] > 1)
			sprintf(tmp, "%s%d(x%d)", comma, msg->cpus_per_node[i],
				msg->cpu_count_reps[i]);
		else
			sprintf(tmp, "%s%d", comma, msg->cpus_per_node[i]);
		xstrcat(task_str, tmp);
	}
	
        return task_str;
}

static void
_send_launch_failure (launch_tasks_request_msg_t *msg, slurm_addr *cli, int rc)
{
	slurm_msg_t resp_msg;
	launch_tasks_response_msg_t resp;

	debug ("sending launch failure message: %s", slurm_strerror (rc));

	memcpy(&resp_msg.address, cli, sizeof(slurm_addr));
	slurm_set_addr(&resp_msg.address, 
		       msg->resp_port[msg->srun_node_id], 
		       NULL); 
	resp_msg.data = &resp;
	resp_msg.msg_type = RESPONSE_LAUNCH_TASKS;
	forward_init(&resp_msg.forward, NULL);
	resp_msg.ret_list = NULL;
	resp_msg.orig_addr.sin_addr.s_addr = 0;
	resp_msg.forward_struct_init = 0;
	
	resp.node_name     = conf->node_name;
	resp.srun_node_id  = msg->srun_node_id;
	resp.return_code   = rc ? rc : -1;
	resp.count_of_pids = 0;

	slurm_send_only_node_msg(&resp_msg);

	return;
}

static void
_send_launch_resp(slurmd_job_t *job, int rc)
{	
	int i;
	slurm_msg_t resp_msg;
	launch_tasks_response_msg_t resp;
	srun_info_t *srun = list_peek(job->sruns);

	if (job->batch || job->spawn_task)
		return;

	debug("Sending launch resp rc=%d", rc);

        resp_msg.address      = srun->resp_addr;
	resp_msg.data         = &resp;
	resp_msg.msg_type     = RESPONSE_LAUNCH_TASKS;
	forward_init(&resp_msg.forward, NULL);
	resp_msg.ret_list = NULL;
	resp_msg.orig_addr.sin_addr.s_addr = 0;
	resp_msg.forward_struct_init = 0;
	
	resp.node_name        = conf->node_name;
	resp.srun_node_id     = job->nodeid;
	resp.return_code      = rc;
	resp.count_of_pids    = job->ntasks;

	resp.local_pids = xmalloc(job->ntasks * sizeof(*resp.local_pids));
	for (i = 0; i < job->ntasks; i++) 
		resp.local_pids[i] = job->task[i]->pid;  

	slurm_send_only_node_msg(&resp_msg);

	xfree(resp.local_pids);
}


static int
_send_complete_batch_script_msg(slurmd_job_t *job, int err, int status)
{
	int                      rc, i;
	slurm_msg_t              req_msg;
	complete_batch_script_msg_t  req;

	req.job_id	= job->jobid;
	req.job_rc      = status;
	req.slurm_rc	= err; 
		
	req.node_name	= conf->node_name;
	req_msg.msg_type= REQUEST_COMPLETE_BATCH_SCRIPT;
	req_msg.data	= &req;	
	forward_init(&req_msg.forward, NULL);
	req_msg.ret_list = NULL;
	req_msg.forward_struct_init = 0;
	
	info("sending REQUEST_COMPLETE_BATCH_SCRIPT");

	/* Note: these log messages don't go to slurmd.log from here */
	for (i=0; i<=MAX_RETRY; i++) {
		if (slurm_send_recv_controller_rc_msg(&req_msg, &rc) == 0)
			break;
		info("Retrying job complete RPC for %u.%u",
		     job->jobid, job->stepid);
		sleep(RETRY_DELAY);
	}
	if (i > MAX_RETRY) {
		error("Unable to send job complete message: %m");
		return SLURM_ERROR;
	}

	if ((rc == ESLURM_ALREADY_DONE) || (rc == ESLURM_INVALID_JOB_ID))
		rc = SLURM_SUCCESS;
	if (rc)
		slurm_seterrno_ret(rc);

	return SLURM_SUCCESS;
}


static int
_drop_privileges(slurmd_job_t *job, bool do_setuid, struct priv_state *ps)
{
	ps->saved_uid = getuid();
	ps->saved_gid = getgid();

	if (!getcwd (ps->saved_cwd, sizeof (ps->saved_cwd))) {
		error ("Unable to get current working directory: %m");
		strncpy (ps->saved_cwd, "/tmp", sizeof (ps->saved_cwd));
	}

	ps->ngids = getgroups(0, NULL);

	ps->gid_list = (gid_t *) xmalloc(ps->ngids * sizeof(gid_t));

	getgroups(ps->ngids, ps->gid_list);

	/*
	 * No need to drop privileges if we're not running as root
	 */
	if (getuid() != (uid_t) 0)
		return SLURM_SUCCESS;

	if (setegid(job->pwd->pw_gid) < 0) {
		error("setegid: %m");
		return -1;
	}

	if (_initgroups(job) < 0) {
		error("_initgroups: %m"); 
	}

	if (do_setuid && seteuid(job->pwd->pw_uid) < 0) {
		error("seteuid: %m");
		return -1;
	}

	return SLURM_SUCCESS;
}

static int
_reclaim_privileges(struct priv_state *ps)
{
	/* 
	 * No need to reclaim privileges if our uid == pwd->pw_uid
	 */
	if (geteuid() == ps->saved_uid)
		return SLURM_SUCCESS;

	if (seteuid(ps->saved_uid) < 0) {
		error("seteuid: %m");
		return -1;
	}

	if (setegid(ps->saved_gid) < 0) {
		error("setegid: %m");
		return -1;
	}

	setgroups(ps->ngids, ps->gid_list);

	xfree(ps->gid_list);

	return SLURM_SUCCESS;
}


static void
_slurmd_job_log_init(slurmd_job_t *job) 
{
	char argv0[64];

	if (!job->spawn_task)
		conf->log_opts.buffered = 1;

	/*
	 * Reset stderr logging to user requested level
	 * (Logfile and syslog levels remain the same)
	 *
	 * The maximum stderr log level is LOG_LEVEL_DEBUG3 because
	 * some higher level debug messages are generated in the
	 * stdio code, which would otherwise create more stderr traffic
	 * to srun and therefore more debug messages in an endless loop.
	 */
	conf->log_opts.stderr_level = LOG_LEVEL_ERROR + job->debug;
	if (conf->log_opts.stderr_level > LOG_LEVEL_DEBUG3)
		conf->log_opts.stderr_level = LOG_LEVEL_DEBUG3;


	snprintf(argv0, sizeof(argv0), "slurmd[%s]", conf->hostname);
	/* 
	 * reinitialize log 
	 */
	
	log_alter(conf->log_opts, 0, NULL);
	log_set_argv0(argv0);
	
	/* Connect slurmd stderr to job's stderr */
	if ((!job->spawn_task) && (job->task != NULL)) {
		if (dup2(job->task[0]->stderr_fd, STDERR_FILENO) < 0) {
			error("job_log_init: dup2(stderr): %m");
			return;
		}
	}
}


static void
_setargs(slurmd_job_t *job)
{
	if (job->jobid > MAX_NOALLOC_JOBID)
		return;

	if ((job->jobid >= MIN_NOALLOC_JOBID) || (job->stepid == NO_VAL))
		setproctitle("[%u]",    job->jobid);
	else
		setproctitle("[%u.%u]", job->jobid, job->stepid); 

	return;
}

/*
 * Set the priority of the job to be the same as the priority of
 * the process that launched the job on the submit node.
 * In support of the "PropagatePrioProcess" config keyword.
 */
static void _set_prio_process (slurmd_job_t *job)
{
	char *env_name = "SLURM_PRIO_PROCESS";
	char *env_val;

	int prio_process;

	if (!(env_val = getenvp( job->env, env_name ))) {
		error( "Couldn't find %s in environment", env_name );
		return;
	}

	/*
	 * Users shouldn't get this in their environ
	 */
	unsetenvp( job->env, env_name );

	prio_process = atoi( env_val );

	if (setpriority( PRIO_PROCESS, 0, prio_process ))
		error( "setpriority(PRIO_PROCESS): %m" );

	debug2( "_set_prio_process: setpriority %d succeeded", prio_process);
}

static int
_become_user(slurmd_job_t *job, struct priv_state *ps)
{
	/*
	 * First reclaim the effective uid and gid
	 */
	if (geteuid() == ps->saved_uid)
		return SLURM_SUCCESS;

	if (seteuid(ps->saved_uid) < 0) {
		error("_become_user seteuid: %m");
		return SLURM_ERROR;
	}

	if (setegid(ps->saved_gid) < 0) {
		error("_become_user setegid: %m");
		return SLURM_ERROR;
	}

	/*
	 * Now drop real, effective, and saved uid/gid
	 */
	if (setregid(job->pwd->pw_gid, job->pwd->pw_gid) < 0) {
		error("setregid: %m");
		return SLURM_ERROR;
	}

	if (setreuid(job->pwd->pw_uid, job->pwd->pw_uid) < 0) {
		error("setreuid: %m");
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}	


static int
_initgroups(slurmd_job_t *job)
{
	int rc;
	char *username;
	gid_t gid;

	if (job->ngids > 0) {
		xassert(job->gids);
		debug2("Using gid list sent by slurmd");
		return setgroups(job->ngids, job->gids);
	}

	username = job->pwd->pw_name;
	gid = job->pwd->pw_gid;
	debug2("Uncached user/gid: %s/%ld", username, (long)gid);
	if ((rc = initgroups(username, gid))) {
		if ((errno == EPERM) && (getuid() != (uid_t) 0)) {
			debug("Error in initgroups(%s, %ld): %m",
				username, (long)gid);
		} else {
			error("Error in initgroups(%s, %ld): %m",
				username, (long)gid);
		}
		return -1;
	}
	return 0;
}