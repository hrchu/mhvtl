/*
 *  linux/kernel/vtl.c
 * vvvvvvvvvvvvvvvvvvvvvvv Original vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
 *  Copyright (C) 1992  Eric Youngdale
 *  Simulate a host adapter with 2 disks attached.  Do a lot of checking
 *  to make sure that we are not getting blocks mixed up, and PANIC if
 *  anything out of the ordinary is seen.
 * ^^^^^^^^^^^^^^^^^^^^^^^ Original ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *  For documentation see http://sg.danny.cz/sg/sdebug26.html
 *
 *   D. Gilbert (dpg) work for Magneto-Optical device test [20010421]
 *   dpg: work for devfs large number of disks [20010809]
 *        forked for lk 2.5 series [20011216, 20020101]
 *        use vmalloc() more inquiry+mode_sense [20020302]
 *   Patrick Mansfield <patmans@us.ibm.com> max_luns+scsi_level [20021031]
 *   Mike Anderson <andmike@us.ibm.com> sysfs work [20021118]
 *   dpg: change style of boot options to "vtl.num_tgts=2" and
 *        module options to "modprobe vtl num_tgts=2" [20021221]
 *
 *	Mark Harvey 2005-6-1
 * 
 *	markh794@gmail.com
 *	  or
 *	Current employ address: mark_harvey@symantec.com
 *
 *	Pinched wholesale from scsi_debug.[ch]
 *
 *	Hacked to represent SCSI tape drives & Library.
 *
 *	Registered char driver to handle data to user space daemon.
 *	Idea is for user space daemons (vxtape & vxlibrary) to emulate
 *	and process the SCSI SSC/SMC device command set.
 *
 *	I've used it for testing NetBackup - but there is no reason any
 *	other backup utility could not use it as well.
 *
 *	Requires Linux kernel 2.6.10 for the 'generic' circular buffer
 *	- My thanks to Stelian Pop
 *
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/smp_lock.h>
#include <linux/moduleparam.h>
#include <asm/uaccess.h>

#include <linux/blkdev.h>
#include <linux/cdev.h>

#include <scsi/scsi_host.h>
#include <scsi/scsicam.h>

#include <linux/stat.h>

#ifndef LINUX_VERSION_CODE
#include <linux/version.h>
#endif

#ifndef _SCSI_H
#define _SCSI_H

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi.h>

struct Scsi_Host;
struct scsi_cmnd;
struct scsi_device;
struct scsi_target;
struct scatterlist;

#endif /* _SCSI_H */

#include "vtl_common.h"

#include <scsi/scsi_driver.h>
#include <scsi/scsi_ioctl.h>

/* version of scsi_debug I started from
 #define VTL_VERSION "1.75"
*/
/* SCSI command definations not covered in default scsi.h */
#define WRITE_ATTRIBUTE 0x8d
#define SECURITY_PROTOCOL_OUT 0xb5
#define VTL_VERSION "0.16.0"
static const char *vtl_version_date = "20090605-0";

/* Additional Sense Code (ASC) used */
#define NO_ADDED_SENSE 0x0
#define INVALID_FIELD_IN_CDB 0x24
#define POWERON_RESET 0x29
#define NOT_SELF_CONFIGURED 0x3e

#define VTL_TAGGED_QUEUING 0 /* 0 | MSG_SIMPLE_TAG | MSG_ORDERED_TAG */

/* Default values for driver parameters */
#define DEF_NUM_HOST   1
#define DEF_NUM_TGTS   0
#define DEF_MAX_LUNS   7
#define DEF_DELAY   1
#define DEF_EVERY_NTH   0
#define DEF_NUM_PARTS   0
#define DEF_OPTS   1		/* Default to verbose logging */
#define DEF_SCSI_LEVEL   5	/* INQUIRY, byte2 [5->SPC-3] */
#define DEF_D_SENSE   0
#define DEF_RETRY_REQUEUE 4	/* How many times to re-try a cmd requeue */

/* bit mask values for vtl_opts */
#define VTL_OPT_NOISE   1
#define VTL_OPT_MEDIUM_ERR   2
#define VTL_OPT_TIMEOUT   4
#define VTL_OPT_RECOVERED_ERR   8
/* When "every_nth" > 0 then modulo "every_nth" commands:
 *   - a no response is simulated if VTL_OPT_TIMEOUT is set
 *   - a RECOVERED_ERROR is simulated on successful read and write
 *     commands if VTL_OPT_RECOVERED_ERR is set.
 *
 * When "every_nth" < 0 then after "- every_nth" commands:
 *   - a no response is simulated if VTL_OPT_TIMEOUT is set
 *   - a RECOVERED_ERROR is simulated on successful read and write
 *     commands if VTL_OPT_RECOVERED_ERR is set.
 * This will continue until some other action occurs (e.g. the user
 * writing a new value (other than -1 or 1) to every_nth via sysfs).
 */

/* If REPORT LUNS has luns >= 256 it can choose "flat space" (value 1)
 * or "peripheral device" addressing (value 0) */
#define SAM2_LUN_ADDRESS_METHOD 0

/* Major number assigned to vtl driver => 0 means to ask for one */
static int vtl_Major = 0;

#define DEF_MAX_MINOR_NO 256	/* Max number of minor nos. this driver will handle */

#define VTL_CANQUEUE  255 	/* needs to be >= 1 */
#define VTL_MAX_CMD_LEN 16

static int vtl_add_host = DEF_NUM_HOST;
static int vtl_every_nth = DEF_EVERY_NTH;
static int vtl_max_luns = DEF_MAX_LUNS;
static int vtl_num_tgts = DEF_NUM_TGTS; /* targets per host */
static int vtl_opts = DEF_OPTS;
static int vtl_scsi_level = DEF_SCSI_LEVEL;
static int vtl_dsense = DEF_D_SENSE;
static int vtl_add_lu = 0;

static int vtl_cmnd_count = 0;

struct vtl_lu_info {
	struct list_head lu_sibling;
	unsigned char sense_buff[SENSE_BUF_SIZE];	/* weak nexus */
	unsigned int channel;
	unsigned int target;
	unsigned int lun;
	unsigned int minor;
	struct vtl_hba_info *vtl_hba;
	struct scsi_device *sdev;

	char reset;
	char device_offline;

	struct semaphore lock;

	struct list_head cmd_list; /* list of outstanding cmds for this lu */
	spinlock_t cmd_list_lock;
};

static struct vtl_lu_info *devp[DEF_MAX_MINOR_NO];

struct vtl_hba_info {
	struct list_head hba_sibling; /* List of adapters */
	struct list_head lu_list; /* List of lu */
	struct Scsi_Host *shost;
	struct device dev;
};

#define to_vtl_hba(d) \
	container_of(d, struct vtl_hba_info, dev)

static LIST_HEAD(vtl_hba_list);	/* dll of adapters */
static spinlock_t vtl_hba_list_lock = SPIN_LOCK_UNLOCKED;

typedef void (* done_funct_t) (struct scsi_cmnd *);

/* vtl_queued_cmd-> state */
enum cmd_state {
	CMD_STATE_FREE = 0,
	CMD_STATE_QUEUED,
	CMD_STATE_IN_USE,
};

struct vtl_queued_cmd {
	int state;
	struct timer_list cmnd_timer;
	done_funct_t done_funct;
	struct scsi_cmnd *a_cmnd;
	int scsi_result;
	struct vtl_header op_header;

	struct list_head queued_sibling;
};

static int num_aborts = 0;
static int num_dev_resets = 0;
static int num_bus_resets = 0;
static int num_host_resets = 0;

static char vtl_driver_name[] = "mhvtl";

static int vtl_driver_probe(struct device *);
static int vtl_driver_remove(struct device *);
static struct bus_type pseudo_lld_bus;

static struct device_driver vtl_driverfs_driver = {
	.name 		= vtl_driver_name,
	.bus		= &pseudo_lld_bus,
	.probe          = vtl_driver_probe,
	.remove         = vtl_driver_remove,
};

static const int check_condition_result =
		(DRIVER_SENSE << 24) | SAM_STAT_CHECK_CONDITION;

/* function declarations */
static int resp_requests(struct scsi_cmnd *SCpnt, struct vtl_lu_info *lu);
static int resp_report_luns(struct scsi_cmnd *SCpnt, struct vtl_lu_info *lu);
static int fill_from_user_buffer(struct scsi_cmnd *scp, char __user *arr,
				int arr_len);
static int fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len);
static void timer_intr_handler(unsigned long);
static struct vtl_lu_info *devInfoReg(struct scsi_device *sdp);
static void mk_sense_buffer(struct vtl_lu_info *lu, int key, int asc, int asq);
static void stop_all_queued(void);
static int do_create_driverfs_files(void);
static void do_remove_driverfs_files(void);

static int vtl_add_adapter(void);
static void vtl_remove_adapter(void);
static void vtl_max_tgts_luns(void);

static int vtl_slave_alloc(struct scsi_device *);
static int vtl_slave_configure(struct scsi_device *);
static void vtl_slave_destroy(struct scsi_device *);
static int vtl_queuecommand(struct scsi_cmnd *,
				   void (*done) (struct scsi_cmnd *));
static int vtl_b_ioctl(struct scsi_device *, int, void __user *);
static int vtl_c_ioctl(struct inode *, struct file *, unsigned int, unsigned long);
static int vtl_abort(struct scsi_cmnd *);
static int vtl_bus_reset(struct scsi_cmnd *);
static int vtl_device_reset(struct scsi_cmnd *);
static int vtl_host_reset(struct scsi_cmnd *);
static int vtl_proc_info(struct Scsi_Host *, char *, char **, off_t, int, int);
static const char * vtl_info(struct Scsi_Host *);
static int vtl_open(struct inode *, struct file *);
static int vtl_release(struct inode *, struct file *);

static struct device pseudo_primary;
static struct bus_type pseudo_lld_bus;

static struct scsi_host_template vtl_driver_template = {
	.proc_info =		vtl_proc_info,
	.name =			"VTL",
	.info =			vtl_info,
	.slave_alloc =		vtl_slave_alloc,
	.slave_configure =	vtl_slave_configure,
	.slave_destroy =	vtl_slave_destroy,
	.ioctl =		vtl_b_ioctl,
	.queuecommand =		vtl_queuecommand,
	.eh_abort_handler =	vtl_abort,
	.eh_bus_reset_handler = vtl_bus_reset,
	.eh_device_reset_handler = vtl_device_reset,
	.eh_host_reset_handler = vtl_host_reset,
	.can_queue =		VTL_CANQUEUE,
	.this_id =		7,
	.sg_tablesize =		64,
	.cmd_per_lun =		7,
	.max_sectors =		4096,
	.unchecked_isa_dma = 	0,
	.use_clustering = 	DISABLE_CLUSTERING,
	.module =		THIS_MODULE,
};

static struct file_operations vtl_fops = {
	.owner   =  THIS_MODULE,
	.ioctl   =  vtl_c_ioctl,
	.open    =  vtl_open,
	.release =  vtl_release,
};


#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,26)
 #include "fetch27.c"
#elif LINUX_VERSION_CODE == KERNEL_VERSION(2,6,26)
 #include "fetch26.c"
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2,6,23)
 #include "fetch24.c"
#else
 #include "fetch.c"
#endif

/**********************************************************************
 *                misc functions to handle queuing SCSI commands
 **********************************************************************/

/*
 * schedule_resp() - handle SCSI commands that are processed from the
 *                   queuecommand() interface. i.e. No callback to done()
 *                   outside the queuecommand() function.
 *
 *                   Any SCSI command handled directly by the kernel driver
 *                   will use this.
 */
static int schedule_resp(struct scsi_cmnd *SCpnt,
			 struct vtl_lu_info *lu,
			 done_funct_t done, int scsi_result)
{
	if ((VTL_OPT_NOISE & vtl_opts) && SCpnt) {
		if (scsi_result) {
			struct scsi_device *sdp = SCpnt->device;

			printk(KERN_INFO "mhvtl:    <%u %u %u %u> "
			       "non-zero result=0x%x\n", sdp->host->host_no,
			       sdp->channel, sdp->id, sdp->lun, scsi_result);
		}
	}
	if (SCpnt && lu) {
		/* simulate autosense by this driver */
		if (SAM_STAT_CHECK_CONDITION == (scsi_result & 0xff))
			memcpy(SCpnt->sense_buffer, lu->sense_buff,
			       (SCSI_SENSE_BUFFERSIZE > SENSE_BUF_SIZE) ?
			       SENSE_BUF_SIZE : SCSI_SENSE_BUFFERSIZE);
	}
	if (SCpnt)
		SCpnt->result = scsi_result;
	if (done)
		done(SCpnt);
	return 0;
}

/*
 * The SCSI error code when the user space daemon is not connected.
 */
static int resp_becomming_ready(struct vtl_lu_info *lu)
{
	mk_sense_buffer(lu, NOT_READY, NOT_SELF_CONFIGURED, NO_ADDED_SENSE);
	return check_condition_result;
}

/**********************************************************************
 *                SCSI data handling routines
 **********************************************************************/
static int resp_write_to_user(struct scsi_cmnd *SCpnt,
			  void __user *up, int count)
{
	int fetched;

	fetched = fetch_to_dev_buffer(SCpnt, up, count);

	if ((fetched < count) && (VTL_OPT_NOISE & vtl_opts))
		printk(KERN_INFO "mhvtl: write: cdb indicated=%d, "
		       " IO sent=%d bytes\n", count, fetched);

	return 0;
}

static void debug_queued_list(struct vtl_lu_info *lu)
{
	unsigned long iflags = 0;
	struct vtl_queued_cmd *sqcp, *n;
	int k = 0;

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry_safe(sqcp, n, &lu->cmd_list, queued_sibling) {
		if (sqcp->state) {
			if (sqcp->a_cmnd) {
				printk("mhvtl: %s %d entry in use "
				"SCpnt: %p, SCSI result: %d, done: %p, "
				"Serial No: %ld\n",
					__func__, k,
					sqcp->a_cmnd, sqcp->scsi_result,
					sqcp->done_funct,
					sqcp->a_cmnd->serial_number);
			} else {
				printk("mhvtl: %s %d entry in use "
				"SCpnt: %p, SCSI result: %d, done: %p\n",
					__func__, k,
					sqcp->a_cmnd, sqcp->scsi_result,
					sqcp->done_funct);
			}
		} else
			printk("mhvtl: %s entry free %d\n", __func__, k);
		k++;
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	printk(KERN_INFO "mhvtl: %s found %d entr%s\n",
			 __func__, k, (k == 1) ? "y" : "ies");
}

static struct vtl_hba_info *vtl_get_hba_entry(void)
{
	struct vtl_hba_info *vtl_hba;

	spin_lock(&vtl_hba_list_lock);
	if (list_empty(&vtl_hba_list))
		vtl_hba = NULL;
	else
		vtl_hba = list_entry(vtl_hba_list.prev,
					struct vtl_hba_info, hba_sibling);
	spin_unlock(&vtl_hba_list_lock);
	return vtl_hba;
}

static void dump_queued_list(void)
{
	struct vtl_lu_info *lu;

	struct vtl_hba_info *vtl_hba;

	vtl_hba = vtl_get_hba_entry();

	/* Now that the work list is split per lu, we have to check each
	 * lu to see if we can find the serial number in question
	 */
	list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling) {
		printk("mhvtl: %s Channel %d, ID %d, LUN %d\n",
				__func__, lu->channel, lu->target, lu->lun);
		debug_queued_list(lu);
	}
}

/*********************************************************
 * Generic interface to queue SCSI cmd to userspace daemon
 *********************************************************/
/*
 * q_cmd returns success if we successfully added the SCSI
 * cmd to the queued_list
 *
 * - Set state to indicate that the SCSI cmnd is ready for processing.
 */
static int q_cmd(struct scsi_cmnd *scp,
				done_funct_t done,
				struct vtl_lu_info *lu)
{
	unsigned long iflags;
	struct vtl_header *vheadp;
	struct vtl_queued_cmd *sqcp;

	/* No user space daemon talking to us */
	if (lu->device_offline) {
		printk("%s device <%d %d %d> Offline: No user-space daemons"
			" registered\n", __func__,
			lu->channel, lu->target, lu->lun);
		return resp_becomming_ready(lu);
	}


	sqcp = kmalloc(sizeof(*sqcp), GFP_ATOMIC);
	if (!sqcp) {
		printk(KERN_WARNING "mhvtl: %s kmalloc failed\n", __func__);
		return 1;
	}

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	init_timer(&sqcp->cmnd_timer);
	list_add_tail(&sqcp->queued_sibling, &lu->cmd_list);
	sqcp->a_cmnd = scp;
	sqcp->scsi_result = 0;
	sqcp->done_funct = done;
	sqcp->cmnd_timer.function = timer_intr_handler;
	sqcp->cmnd_timer.data = scp->serial_number;
	sqcp->cmnd_timer.expires = jiffies + 25000;
	add_timer(&sqcp->cmnd_timer);
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	if (VTL_OPT_NOISE & vtl_opts)
		dump_queued_list();

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);

	vheadp = &sqcp->op_header;
	vheadp->serialNo = scp->serial_number;
	memcpy(vheadp->cdb, scp->cmnd, scp->cmd_len);

	/* Set flag.
	 * Next ioctl() poll by user-daemon will check this state.
	 */
	sqcp->state = CMD_STATE_QUEUED;

	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);

	return 0;
}

/**********************************************************************
 *                Main interface from SCSI mid level
 **********************************************************************/
static int vtl_queuecommand(struct scsi_cmnd *SCpnt, done_funct_t done)
{
	unsigned char *cmd = (unsigned char *) SCpnt->cmnd;
	int num;
	int k;
	int errsts = 0;
	struct vtl_lu_info *lu = NULL;
	int inj_recovered = 0;

	if (done == NULL)
		return 0;	/* assume mid level reprocessing command */

	if ((VTL_OPT_NOISE & vtl_opts) && cmd) {
//		if (TEST_UNIT_READY != cmd[0]) {	// Skip TUR *
			printk(KERN_INFO "mhvtl: SCSI cdb ");
			for (k = 0, num = SCpnt->cmd_len; k < num; ++k)
				printk("%02x ", (int)cmd[k]);
			printk("\n");
//		}
	}

	if (SCpnt->device->id == vtl_driver_template.this_id) {
		printk(KERN_INFO "mhvtl: initiator's id used as target!\n");
		return schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	if (SCpnt->device->lun >= vtl_max_luns) {
		printk("mhvtl: %s max luns exceeded\n", __func__);
		return schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	lu = devInfoReg(SCpnt->device);
	if (NULL == lu) {
		printk("mhvtl: %s could not find lu\n", __func__);
		return schedule_resp(SCpnt, NULL, done, DID_NO_CONNECT << 16);
	}

	if ((vtl_every_nth != 0) &&
			(++vtl_cmnd_count >= abs(vtl_every_nth))) {
		vtl_cmnd_count = 0;
		if (vtl_every_nth < -1)
			vtl_every_nth = -1;
		if (VTL_OPT_TIMEOUT & vtl_opts)
			return 0; /* ignore command causing timeout */
		else if (VTL_OPT_RECOVERED_ERR & vtl_opts)
			inj_recovered = 1; /* to reads and writes below */
	}

	switch (*cmd) {
	case REQUEST_SENSE:	/* mandatory, ignore unit attention */
		if (lu->device_offline) {
			/* internal REQUEST SENSE routine */
			errsts = resp_requests(SCpnt, lu);
		} else {
			/* User space REQUEST SENSE */
			errsts = q_cmd(SCpnt, done, lu);
			if (errsts == 0)
				return 0;
		}
		break;
	case REPORT_LUNS:	/* mandatory, ignore unit attention */
		errsts = resp_report_luns(SCpnt, lu);
		break;

	/* All commands down the list are handled by a user-space daemon */
	default:	// Pass on to user space daemon to process
		errsts = q_cmd(SCpnt, done, lu);
		if (!errsts)
			return 0;
		break;
	}
	return schedule_resp(SCpnt, lu, done, errsts);
}

static struct vtl_queued_cmd *lookup_sqcp(struct vtl_lu_info *lu,
						unsigned long serialNo)
{
	unsigned long iflags;
	struct vtl_queued_cmd *sqcp;

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry(sqcp, &lu->cmd_list, queued_sibling) {
		if (sqcp->state && (sqcp->a_cmnd->serial_number == serialNo)) {
			spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
			return sqcp;
		}
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	return NULL;
}

/*
 * Block device ioctl
 */
static int vtl_b_ioctl(struct scsi_device *sdp, int cmd, void __user *arg)
{
	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "mhvtl: ioctl: cmd=0x%x\n", cmd);
	}
	return -ENOTTY;
}

static int resp_requests(struct scsi_cmnd *scp, struct vtl_lu_info *lu)
{
	unsigned char *sbuff;
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	unsigned char arr[SENSE_BUF_SIZE];
	int len = 18;

	memset(arr, 0, SENSE_BUF_SIZE);
	if (lu->reset == 1)
		mk_sense_buffer(lu, 0, NO_ADDED_SENSE, 0);
	sbuff = lu->sense_buff;
	if ((cmd[1] & 1) && (!vtl_dsense)) {
		/* DESC bit set and sense_buff in fixed format */
		arr[0] = 0x72;
		arr[1] = sbuff[2];     /* sense key */
		arr[2] = sbuff[12];    /* asc */
		arr[3] = sbuff[13];    /* ascq */
		len = 8;
	} else
		memcpy(arr, sbuff, SENSE_BUF_SIZE);
	mk_sense_buffer(lu, 0, NO_ADDED_SENSE, 0);
	return fill_from_dev_buffer(scp, arr, len);
}

#define MHVTL_RLUN_ARR_SZ 128

static int resp_report_luns(struct scsi_cmnd *scp, struct vtl_lu_info *lu)
{
	unsigned int alloc_len;
	int lun_cnt, i, upper;
	unsigned char *cmd = (unsigned char *)scp->cmnd;
	int select_report = (int)cmd[2];
	struct scsi_lun *one_lun;
	unsigned char arr[MHVTL_RLUN_ARR_SZ];

	alloc_len = cmd[9] + (cmd[8] << 8) + (cmd[7] << 16) + (cmd[6] << 24);
	if ((alloc_len < 16) || (select_report > 2)) {
		mk_sense_buffer(lu, ILLEGAL_REQUEST, INVALID_FIELD_IN_CDB,0);
		return check_condition_result;
	}
	/* can produce response with up to 16k luns (lun 0 to lun 16383) */
	memset(arr, 0, MHVTL_RLUN_ARR_SZ);
	lun_cnt = vtl_max_luns;
	arr[2] = ((sizeof(struct scsi_lun) * lun_cnt) >> 8) & 0xff;
	arr[3] = (sizeof(struct scsi_lun) * lun_cnt) & 0xff;
	lun_cnt = min((int)((MHVTL_RLUN_ARR_SZ - 8) /
			    sizeof(struct scsi_lun)), lun_cnt);
	one_lun = (struct scsi_lun *) &arr[8];
	for (i = 0; i < lun_cnt; i++) {
		upper = (i >> 8) & 0x3f;
		if (upper)
			one_lun[i].scsi_lun[0] =
			    (upper | (SAM2_LUN_ADDRESS_METHOD << 6));
		one_lun[i].scsi_lun[1] = i & 0xff;
	}
	return fill_from_dev_buffer(scp, arr, min((int)alloc_len, MHVTL_RLUN_ARR_SZ));
}

static void __remove_sqcp(struct vtl_queued_cmd *sqcp)
{
	list_del(&sqcp->queued_sibling);
	kfree(sqcp);
}


static void remove_sqcp(struct vtl_lu_info *lu, struct vtl_queued_cmd *sqcp)
{
	unsigned long iflags;
	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	__remove_sqcp(sqcp);
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
}

/* When timer goes off this function is called. */
static void timer_intr_handler(unsigned long indx)
{
	struct vtl_queued_cmd *sqcp = NULL;
	struct vtl_lu_info *lu;

	struct vtl_hba_info *vtl_hba;

	vtl_hba = vtl_get_hba_entry();
	if (!vtl_hba)
		return;

	/* Now that the work list is split per lu, we have to check each
	 * lu to see if we can find the serial number in question
	 */
	list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling) {
		sqcp = lookup_sqcp(lu, indx);
		if (sqcp)
			break;
	}

	if (!sqcp) {
		printk(KERN_ERR "mhvtl: %s: Unexpected interrupt, indx %ld\n",
					 __func__, indx);
		return;
	}

	sqcp->state = CMD_STATE_FREE;
	if (sqcp->done_funct) {
		sqcp->a_cmnd->result = sqcp->scsi_result;
		sqcp->done_funct(sqcp->a_cmnd); /* callback to mid level */
	}
	sqcp->done_funct = NULL;
	remove_sqcp(lu, sqcp);
}

static int vtl_slave_alloc(struct scsi_device *sdp)
{
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu = (struct vtl_lu_info *)sdp->hostdata;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: slave_alloc <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);

	if (lu)
		return 0;

	vtl_hba = *(struct vtl_hba_info **) sdp->host->hostdata;
	if (!vtl_hba) {
		printk(KERN_ERR "Host info NULL\n");
		return -1;
	}

	list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling) {
		if ((!lu->device_offline) &&
				(lu->channel == sdp->channel) &&
				(lu->target == sdp->id) &&
				(lu->lun == sdp->lun)) {
			if (VTL_OPT_NOISE & vtl_opts)
				printk("mhvtl: %s line %d found matching lu\n",
					__func__, __LINE__);
			return 0;
		}
	}
	return -1;
}

static int vtl_slave_configure(struct scsi_device *sdp)
{
	struct vtl_lu_info *lu;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: slave_configure <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);
	if (sdp->host->max_cmd_len != VTL_MAX_CMD_LEN)
		sdp->host->max_cmd_len = VTL_MAX_CMD_LEN;
	lu = devInfoReg(sdp);
	sdp->hostdata = lu;
	if (sdp->host->cmd_per_lun)
		scsi_adjust_queue_depth(sdp, VTL_TAGGED_QUEUING,
					sdp->host->cmd_per_lun);
	return 0;
}

static void vtl_slave_destroy(struct scsi_device *sdp)
{
	struct vtl_lu_info *lu = (struct vtl_lu_info *)sdp->hostdata;
	int minor;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: slave_destroy <%u %u %u %u>\n",
		       sdp->host->host_no, sdp->channel, sdp->id, sdp->lun);
	if (lu) {
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: %s removing lu structure, minor %d\n",
				 __func__, lu->minor);
		/* make this slot avaliable for re-use */
		lu->device_offline = 1;
		minor = lu->minor;
		devp[minor] = NULL;
		kfree(sdp->hostdata);
		sdp->hostdata = NULL;
	}
}

static struct vtl_lu_info *devInfoReg(struct scsi_device *sdp)
{
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu = (struct vtl_lu_info *)sdp->hostdata;

	if (lu)
		return lu;

	vtl_hba = *(struct vtl_hba_info **) sdp->host->hostdata;
	if (!vtl_hba) {
		printk(KERN_ERR "mhvtl: %s Host info NULL\n", __func__);
		return NULL;
	}

	list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling) {
		if ((!lu->device_offline) &&
				(lu->channel == sdp->channel) &&
				(lu->target == sdp->id) &&
				(lu->lun == sdp->lun))
			return lu;
	}

	return NULL;
}

static void mk_sense_buffer(struct vtl_lu_info *lu, int key,
			    int asc, int asq)
{
	unsigned char *sbuff;

	sbuff = lu->sense_buff;
	memset(sbuff, 0, SENSE_BUF_SIZE);
	if (vtl_dsense) {
		sbuff[0] = 0x72;  /* descriptor, current */
		sbuff[1] = key;
		sbuff[2] = asc;
		sbuff[3] = asq;
	} else {
		sbuff[0] = 0x70;  /* fixed, current */
		sbuff[2] = key;
		sbuff[7] = 0xa;	  /* implies 18 byte sense buffer */
		sbuff[12] = asc;
		sbuff[13] = asq;
	}
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl:    [sense_key,asc,ascq]: "
		      "[0x%x,0x%x,0x%x]\n", key, asc, asq);
}

static int vtl_device_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_lu_info *lu;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: %s()\n", __func__);
	++num_dev_resets;
	if (SCpnt) {
		lu = devInfoReg(SCpnt->device);
		if (lu)
			lu->reset = 1;
	}
	return SUCCESS;
}

static int vtl_bus_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu;
	struct scsi_device *sdp;
	struct Scsi_Host *hp;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: bus_reset\n");
	++num_bus_resets;
	if (SCpnt && ((sdp = SCpnt->device)) && ((hp = sdp->host))) {
		vtl_hba = *(struct vtl_hba_info **) hp->hostdata;
		if (vtl_hba) {
			list_for_each_entry(lu, &vtl_hba->lu_list,
						lu_sibling)
			lu->reset = 1;
		}
	}
	return SUCCESS;
}

static int vtl_host_reset(struct scsi_cmnd *SCpnt)
{
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu;

	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: host_reset\n");
	++num_host_resets;
	spin_lock(&vtl_hba_list_lock);
	list_for_each_entry(vtl_hba, &vtl_hba_list, hba_sibling) {
		list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling)
		lu->reset = 1;
	}
	spin_unlock(&vtl_hba_list_lock);
	stop_all_queued();
	return SUCCESS;
}

/* Returns 1 if found 'cmnd' and deleted its timer. else returns 0 */
static int stop_queued_cmnd(struct scsi_cmnd *SCpnt)
{
	int found = 0;
	unsigned long iflags;
	struct vtl_queued_cmd *sqcp, *n;
	struct vtl_lu_info *lu;

	lu = devInfoReg(SCpnt->device);

	spin_lock_irqsave(&lu->cmd_list_lock, iflags);
	list_for_each_entry_safe(sqcp, n, &lu->cmd_list, queued_sibling) {
		if (sqcp->state && (SCpnt == sqcp->a_cmnd)) {
			del_timer_sync(&sqcp->cmnd_timer);
			sqcp->state = CMD_STATE_FREE;
			sqcp->a_cmnd = NULL;
			found = 1;
			__remove_sqcp(sqcp);
			break;
		}
	}
	spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	return found;
}

/* Deletes (stops) timers of all queued commands */
static void stop_all_queued(void)
{
	unsigned long iflags;
	struct vtl_queued_cmd *sqcp, *n;
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu;

	vtl_hba = vtl_get_hba_entry();

	list_for_each_entry(lu, &vtl_hba->lu_list, lu_sibling) {
		spin_lock_irqsave(&lu->cmd_list_lock, iflags);
		list_for_each_entry_safe(sqcp, n, &lu->cmd_list, queued_sibling) {
			if (sqcp->state && sqcp->a_cmnd) {
				del_timer_sync(&sqcp->cmnd_timer);
				sqcp->state = CMD_STATE_FREE;
				sqcp->a_cmnd = NULL;
				__remove_sqcp(sqcp);
			}
		}
		spin_unlock_irqrestore(&lu->cmd_list_lock, iflags);
	}
}

static int vtl_abort(struct scsi_cmnd *SCpnt)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: %s()\n", __func__);
	++num_aborts;
	stop_queued_cmnd(SCpnt);
	return SUCCESS;
}

/* SLES 9 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,6)
struct scsi_device *__scsi_add_device(struct Scsi_Host *hpnt, uint channel, uint id, uint lun, char *p )
{
	return scsi_add_device(hpnt, channel, id, lun);
}
#endif

/*
 * According to scsi_mid_low_api.txt
 *
 * A call from LLD scsi_add_device() will result in SCSI mid layer
 *   -> slave_alloc()
 *   -> slave_configure()
 */
static int vtl_add_device(int minor, struct vtl_ctl *ctl)
{
	struct Scsi_Host *hpnt;
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu;
	int error = 0;

	if (devp[minor]) {
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: %s dev structure already in place\n",
						__func__);
		return error;
	}

	vtl_hba = vtl_get_hba_entry();
	if (!vtl_hba) {
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: %s vtl_ost_info struct is NULL\n",
						__func__);
		return -ENOTTY;
	}
	if (VTL_OPT_NOISE & vtl_opts)
		printk("mhvtl: %s vtl_hba_info struct is %p\n",
						__func__, vtl_hba);

	hpnt = vtl_hba->shost;
	if (!hpnt) {
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: %s scsi host structure is NULL\n",
						__func__);
		return -ENOTTY;
	}
	if (VTL_OPT_NOISE & vtl_opts)
		printk("mhvtl: %s scsi_host struct is %p\n",
						__func__, hpnt);

	lu = kmalloc(sizeof(*lu), GFP_KERNEL);
	if (!lu) {
		printk(KERN_ERR "mhvtl: %s line %d - out of memory\n",
						__func__, __LINE__);
		return -ENOMEM;
	}
	memset(lu, 0, sizeof(*lu));
	list_add_tail(&lu->lu_sibling, &vtl_hba->lu_list);

	lu->minor = minor;
	lu->channel = ctl->channel;
	lu->target = ctl->id;
	lu->lun = ctl->lun;
	lu->vtl_hba = vtl_hba;
	lu->reset = 0;
	lu->device_offline = 0;
	lu->cmd_list_lock = SPIN_LOCK_UNLOCKED;

	/* List of queued SCSI op codes associated with this device */
	INIT_LIST_HEAD(&lu->cmd_list);

	init_MUTEX(&lu->lock);

	if (vtl_dsense)
		lu->sense_buff[0] = 0x72;
	else {
		lu->sense_buff[0] = 0x70;
		lu->sense_buff[7] = 0xa;
	}
	devp[minor] = lu;
	if (VTL_OPT_NOISE & vtl_opts)
		printk("mhvtl: %s Added lu: %p to devp[%d]\n",
						__func__, lu, minor);

	lu->sdev = __scsi_add_device(hpnt, ctl->channel, ctl->id, ctl->lun, NULL);
	if (IS_ERR(lu->sdev)) {
		lu->sdev = NULL;
		error = -ENODEV;
	}
	return error;
}

/* Set 'perm' (4th argument) to 0 to disable module_param's definition
 * of sysfs parameters (which module_param doesn't yet support).
 * Sysfs parameters defined explicitly below.
 */
module_param_named(dsense, vtl_dsense, int, 0);
module_param_named(every_nth, vtl_every_nth, int, 0);
module_param_named(max_luns, vtl_max_luns, int, 0);
module_param_named(num_tgts, vtl_num_tgts, int, 0);
module_param_named(opts, vtl_opts, int, 0); /* perm=0644 */
module_param_named(scsi_level, vtl_scsi_level, int, 0);
module_param_named(add_lu, vtl_add_lu, int, 0);

MODULE_AUTHOR("Eric Youngdale + Douglas Gilbert + Mark Harvey");
MODULE_DESCRIPTION("SCSI vtl adapter driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(VTL_VERSION);

MODULE_PARM_DESC(dsense, "use descriptor sense format(def: fixed)");
MODULE_PARM_DESC(every_nth, "timeout every nth command(def=100)");
MODULE_PARM_DESC(max_luns, "number of SCSI LUNs per target to simulate");
MODULE_PARM_DESC(num_tgts, "number of SCSI targets per host to simulate");
MODULE_PARM_DESC(opts, "1->noise, 2->medium_error, 4->...");
MODULE_PARM_DESC(scsi_level, "SCSI level to simulate(def=5[SPC-3])");
MODULE_PARM_DESC(add_lu, "Initiate adding logical unit defined by: "
			"minor, channel, target, lun");


static char vtl_parm_info[256];

static const char *vtl_info(struct Scsi_Host *shp)
{
	sprintf(vtl_parm_info, "mhvtl: version %s [%s], "
		"opts=0x%x", VTL_VERSION,
		vtl_version_date, vtl_opts);
	return vtl_parm_info;
}

/* vtl_proc_info
 * Used if the driver currently has no own support for /proc/scsi
 */
static int vtl_proc_info(struct Scsi_Host *host, char *buffer,
			 char **start, off_t offset, int length, int inout)
{
	int len, pos, begin;
	int orig_length;

	orig_length = length;

	if (inout == 1) {
		char arr[16];
		int minLen = length > 15 ? 15 : length;

		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
			return -EACCES;
		memcpy(arr, buffer, minLen);
		arr[minLen] = '\0';
		if (1 != sscanf(arr, "%d", &pos))
			return -EINVAL;
		vtl_opts = pos;
		if (vtl_every_nth != 0)
			vtl_cmnd_count = 0;
		return length;
	}
	begin = 0;
	pos = len = sprintf(buffer, "mhvtl adapter driver, version "
		"%s [%s]\n"
		"num_tgts=%d, opts=0x%x, "
		"every_nth=%d(curr:%d)\n"
		"max_luns=%d,"
		"scsi_level=%d\n"
		"number of aborts=%d, device_reset=%d, bus_resets=%d, "
		"host_resets=%d \n",
		VTL_VERSION, vtl_version_date, vtl_num_tgts,
		vtl_opts, vtl_every_nth,
		vtl_cmnd_count,
		vtl_max_luns,
		vtl_scsi_level,
		num_aborts, num_dev_resets, num_bus_resets, num_host_resets
		);
	if (pos < offset) {
		len = 0;
		begin = pos;
	}
	*start = buffer + (offset - begin);	/* Start of wanted data */
	len -= (offset - begin);
	if (len > length)
		len = length;
	return len;
}

static ssize_t vtl_opts_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n", vtl_opts);
}

static ssize_t vtl_opts_store(struct device_driver *ddp,
				 const char *buf, size_t count)
{
	int opts;
	char work[20];

	if (1 == sscanf(buf, "%10s", work)) {
		if (0 == strnicmp(work,"0x", 2)) {
			if (1 == sscanf(&work[2], "%x", &opts))
				goto opts_done;
		} else {
			if (1 == sscanf(work, "%d", &opts))
				goto opts_done;
		}
	}
	return -EINVAL;
opts_done:
	vtl_opts = opts;
	vtl_cmnd_count = 0;
	return count;
}
DRIVER_ATTR(opts, S_IRUGO | S_IWUSR, vtl_opts_show,
	    vtl_opts_store);

static ssize_t vtl_dsense_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_dsense);
}
static ssize_t vtl_dsense_store(struct device_driver *ddp,
				  const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_dsense = n;
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(dsense, S_IRUGO | S_IWUSR, vtl_dsense_show,
	    vtl_dsense_store);

static ssize_t vtl_num_tgts_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_num_tgts);
}
static ssize_t vtl_num_tgts_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_num_tgts = n;
		vtl_max_tgts_luns();
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(num_tgts, S_IRUGO | S_IWUSR, vtl_num_tgts_show,
	    vtl_num_tgts_store);


static ssize_t vtl_every_nth_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_every_nth);
}
static ssize_t vtl_every_nth_store(struct device_driver *ddp,
				      const char *buf, size_t count)
{
	int nth;

	if ((count > 0) && (1 == sscanf(buf, "%d", &nth))) {
		vtl_every_nth = nth;
		vtl_cmnd_count = 0;
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(every_nth, S_IRUGO | S_IWUSR, vtl_every_nth_show,
	    vtl_every_nth_store);

static ssize_t vtl_max_luns_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_max_luns);
}
static ssize_t vtl_max_luns_store(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int n;

	if ((count > 0) && (1 == sscanf(buf, "%d", &n)) && (n >= 0)) {
		vtl_max_luns = n;
		vtl_max_tgts_luns();
		return count;
	}
	return -EINVAL;
}
DRIVER_ATTR(max_luns, S_IRUGO | S_IWUSR, vtl_max_luns_show,
	    vtl_max_luns_store);

static ssize_t vtl_scsi_level_show(struct device_driver *ddp, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", vtl_scsi_level);
}
DRIVER_ATTR(scsi_level, S_IRUGO, vtl_scsi_level_show, NULL);

static ssize_t vtl_add_lu_action(struct device_driver *ddp,
				     const char *buf, size_t count)
{
	int retval;
	int minor;
	struct vtl_ctl ctl;
	char str[512];

	if (strncmp(buf, "add", 3)) {
		printk("mhvtl: %s Invalid command: %s\n", __func__, buf);
		return count;
	}

	retval = sscanf(buf, "%s %d %d %d %d",
			str, &minor, &ctl.channel, &ctl.id, &ctl.lun);

	if (VTL_OPT_NOISE & vtl_opts)
		printk("mhvtl: %s 'vtl_add_device(minor: %d,"
			" Channel: %d, ID: %d, LUN: %d)\n",
			__func__,
			minor, ctl.channel, ctl.id, ctl.lun);

	retval = vtl_add_device(minor, &ctl);

	return count;
}
DRIVER_ATTR(add_lu, S_IWUSR|S_IWGRP, NULL, vtl_add_lu_action);

static int do_create_driverfs_files(void)
{
	int	ret;
	ret = driver_create_file(&vtl_driverfs_driver, &driver_attr_add_lu);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_every_nth);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_max_luns);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_num_tgts);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_opts);
	ret |= driver_create_file(&vtl_driverfs_driver, &driver_attr_scsi_level);
	return ret;
}

static void do_remove_driverfs_files(void)
{
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_scsi_level);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_opts);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_num_tgts);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_max_luns);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_every_nth);
	driver_remove_file(&vtl_driverfs_driver, &driver_attr_add_lu);
}

static int __init mhvtl_init(void)
{
	int host_to_add;
	int ret;

	memset(&devp, 0, sizeof(devp));

	vtl_Major = register_chrdev(vtl_Major, "mhvtl", &vtl_fops);
	if (vtl_Major < 0) {
		printk(KERN_WARNING "mhvtl: can't get major number\n");
		return vtl_Major;
	}

	ret = device_register(&pseudo_primary);
	if (ret < 0) {
		printk(KERN_WARNING "mhvtl: device_register error: %d\n", ret);
		goto dev_unreg;
	}
	ret = bus_register(&pseudo_lld_bus);
	if (ret < 0) {
		printk(KERN_WARNING "mhvtl: bus_register error: %d\n", ret);
		goto bus_unreg;
	}
	ret = driver_register(&vtl_driverfs_driver);
	if (ret < 0) {
		printk(KERN_WARNING "mhvtl: driver_register error: %d\n", ret);
		goto driver_unreg;
	}
	ret = do_create_driverfs_files();
	if (ret < 0) {
		printk(KERN_WARNING "mhvtl: driver_create_file error: %d\n", ret);
		goto del_files;
	}

	vtl_driver_template.proc_name = (char *)vtl_driver_name;

	host_to_add = vtl_add_host;
	vtl_add_host = 0;

	if (vtl_add_adapter()) {
		printk(KERN_ERR "mhvtl: %s vtl_add_adapter failed\n", __func__);
		goto del_files;
	}

	if (VTL_OPT_NOISE & vtl_opts) {
		printk(KERN_INFO "%s: built %d host%s\n",
		       __func__, vtl_add_host, (vtl_add_host == 1) ? "" : "s");
	}
	return 0;
del_files:
	do_remove_driverfs_files();
driver_unreg:
	driver_unregister(&vtl_driverfs_driver);
bus_unreg:
	bus_unregister(&pseudo_lld_bus);
dev_unreg:
	device_unregister(&pseudo_primary);

	return ret;
}

static void __exit vtl_exit(void)
{
	int k = vtl_add_host;

	stop_all_queued();
	for (; k; k--)
		vtl_remove_adapter();
	do_remove_driverfs_files();
	driver_unregister(&vtl_driverfs_driver);
	bus_unregister(&pseudo_lld_bus);
	device_unregister(&pseudo_primary);
	unregister_chrdev(vtl_Major, "mhvtl");
}

device_initcall(mhvtl_init);
module_exit(vtl_exit);

void pseudo_0_release(struct device *dev)
{
	if (VTL_OPT_NOISE & vtl_opts)
		printk(KERN_INFO "mhvtl: pseudo_0_release() called\n");
}

static struct device pseudo_primary = {
	.bus_id		= "pseudo_0",
	.release	= pseudo_0_release,
};

static int pseudo_lld_bus_match(struct device *dev,
				struct device_driver *dev_driver)
{
	return 1;
}

static struct bus_type pseudo_lld_bus = {
	.name = "pseudo",
	.match = pseudo_lld_bus_match,
};

static void vtl_release_adapter(struct device *dev)
{
	struct vtl_hba_info *vtl_hba;

	vtl_hba = to_vtl_hba(dev);
	kfree(vtl_hba);
}

/* Simplified from original.
 *
 * Changed so it only adds one hba instance and no logical units
 */
static int vtl_add_adapter(void)
{
	int error = 0;
	struct vtl_hba_info *vtl_hba;

	vtl_hba = kmalloc(sizeof(*vtl_hba), GFP_KERNEL);

	if (!vtl_hba) {
		printk(KERN_ERR "%s: out of memory at line %d\n",
						__func__, __LINE__);
		return -ENOMEM;
	}

	memset(vtl_hba, 0, sizeof(*vtl_hba));
	INIT_LIST_HEAD(&vtl_hba->lu_list);

	spin_lock(&vtl_hba_list_lock);
	list_add_tail(&vtl_hba->hba_sibling, &vtl_hba_list);
	spin_unlock(&vtl_hba_list_lock);

	vtl_hba->dev.bus = &pseudo_lld_bus;
	vtl_hba->dev.parent = &pseudo_primary;
	vtl_hba->dev.release = &vtl_release_adapter;
	sprintf(vtl_hba->dev.bus_id, "adapter%d", vtl_add_host);

	error = device_register(&vtl_hba->dev);
	if (error) {
		kfree(vtl_hba);
		return error;
	}

	vtl_add_host++;
	return error;
}

static void vtl_remove_adapter(void)
{
	struct vtl_hba_info *vtl_hba = NULL;

	spin_lock(&vtl_hba_list_lock);
	if (!list_empty(&vtl_hba_list)) {
		vtl_hba = list_entry(vtl_hba_list.prev,
					struct vtl_hba_info, hba_sibling);
		list_del(&vtl_hba->hba_sibling);
	}
	spin_unlock(&vtl_hba_list_lock);

	if (!vtl_hba)
		return;

	device_unregister(&vtl_hba->dev);
	--vtl_add_host;
}

static int vtl_driver_probe(struct device *dev)
{
	int error = 0;
	struct vtl_hba_info *vtl_hba;
	struct Scsi_Host *hpnt;

	vtl_hba = to_vtl_hba(dev);

	hpnt = scsi_host_alloc(&vtl_driver_template, sizeof(*vtl_hba));
	if (NULL == hpnt) {
		printk(KERN_ERR "%s: scsi_register failed\n", __func__);
		error = -ENODEV;
		return error;
	}

	vtl_hba->shost = hpnt;
	*((struct vtl_hba_info **)hpnt->hostdata) = vtl_hba;
	if ((hpnt->this_id >= 0) && (vtl_num_tgts > hpnt->this_id))
		hpnt->max_id = vtl_num_tgts + 1;
	else
		hpnt->max_id = vtl_num_tgts;
	hpnt->max_lun = vtl_max_luns;

	error = scsi_add_host(hpnt, &vtl_hba->dev);
	if (error) {
		printk(KERN_ERR "%s: scsi_add_host failed\n", __func__);
		error = -ENODEV;
		scsi_host_put(hpnt);
	} else
		scsi_scan_host(hpnt);

	return error;
}

static int vtl_driver_remove(struct device *dev)
{
	struct list_head *lh, *lh_sf;
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu;

	vtl_hba = to_vtl_hba(dev);

	if (!vtl_hba) {
		printk(KERN_ERR "%s: Unable to locate host info\n", __func__);
		return -ENODEV;
	}

	scsi_remove_host(vtl_hba->shost);

	list_for_each_safe(lh, lh_sf, &vtl_hba->lu_list) {
		lu = list_entry(lh, struct vtl_lu_info,
					lu_sibling);
		list_del(&lu->lu_sibling);
		kfree(lu);
	}

	scsi_host_put(vtl_hba->shost);
	vtl_hba->shost = NULL;
	return 0;
}

static void vtl_max_tgts_luns(void)
{
	struct vtl_hba_info *vtl_hba;
	struct Scsi_Host *hpnt;

	spin_lock(&vtl_hba_list_lock);
	list_for_each_entry(vtl_hba, &vtl_hba_list, hba_sibling) {
		hpnt = vtl_hba->shost;
		if ((hpnt->this_id >= 0) &&
		    (vtl_num_tgts > hpnt->this_id))
			hpnt->max_id = vtl_num_tgts + 1;
		else
			hpnt->max_id = vtl_num_tgts;
		hpnt->max_lun = vtl_max_luns;
	}
	spin_unlock(&vtl_hba_list_lock);
}

/*
 *******************************************************************
 * Char device driver routines
 *******************************************************************
 */
static int get_user_data(int minor, char __user *arg)
{
	struct vtl_queued_cmd *sqcp = NULL;
	struct vtl_ds ds;
	int ret = 0;
	unsigned char __user *up;
	size_t sz;

	if (copy_from_user((u8 *)&ds, (u8 *)arg, sizeof(struct vtl_ds)))
		return -EFAULT;

	if (VTL_OPT_NOISE & vtl_opts) {
		printk("%s: data Cmd S/No : %ld\n",
					__func__, (long)ds.serialNo);
		printk(" data pointer     : %p\n", ds.data);
		printk(" data sz          : %d\n", ds.sz);
		printk(" SAM status       : %d (0x%02x)\n",
					ds.sam_stat, ds.sam_stat);
	}
	up = ds.data;
	sz = ds.sz;
	sqcp = lookup_sqcp(devp[minor], ds.serialNo);
	if (!sqcp)
		return -ENOTTY;

	ret = resp_write_to_user(sqcp->a_cmnd, up, sz);

	return ret;
}

static int put_user_data(int minor, char __user *arg)
{
	struct vtl_queued_cmd *sqcp = NULL;
	struct vtl_ds ds;
	int ret = 0;

	if (copy_from_user((u8 *)&ds, (u8 *)arg, sizeof(struct vtl_ds))) {
		ret = -EFAULT;
		goto give_up;
	}
	if (VTL_OPT_NOISE & vtl_opts) {
		printk("%s: data Cmd S/No : %ld\n",
						__func__, (long)ds.serialNo);
		printk(" data pointer     : %p\n", ds.data);
		printk(" data sz          : %d\n", ds.sz);
		printk(" SAM status       : %d (0x%02x)\n",
						ds.sam_stat, ds.sam_stat);
	}
	sqcp = lookup_sqcp(devp[minor], ds.serialNo);
	if (!sqcp) {
		printk(KERN_WARNING "%s: callback function not found for "
				"SCSI cmd s/no. %ld\n",
				__func__, (long)ds.serialNo);
		ret = 1;	/* report busy to mid level */
		goto give_up;
	}
	if (ds.sz)
		ret = fill_from_user_buffer(sqcp->a_cmnd, ds.data, ds.sz);
	if (ds.sam_stat) { /* Auto-sense */
		sqcp->a_cmnd->result = ds.sam_stat;
		if (copy_from_user(sqcp->a_cmnd->sense_buffer,
						ds.sense_buf, SENSE_BUF_SIZE))
			printk("Failed to retrieve autosense data\n");
	} else
		sqcp->a_cmnd->result = DID_OK << 16;
	del_timer_sync(&sqcp->cmnd_timer);
	if (sqcp->done_funct)
		sqcp->done_funct(sqcp->a_cmnd);
	else
		printk("%s FATAL, line %d: SCSI done_funct callback => NULL\n",
						__func__, __LINE__);
	remove_sqcp(devp[minor], sqcp);

	ret = 0;

give_up:
	return ret;
}

static int send_vtl_header(int minor, char __user *arg)
{
	struct vtl_header *vheadp;
	struct vtl_queued_cmd *sqcp;
	int ret = 0;

	list_for_each_entry(sqcp, &devp[minor]->cmd_list, queued_sibling) {
		if (sqcp->state == CMD_STATE_QUEUED) {
			vheadp = &sqcp->op_header;
			if (copy_to_user((u8 *)arg, (u8 *)vheadp,
						sizeof(struct vtl_header))) {
				ret = -EFAULT;
				goto give_up;
			}
			/* Found an outstanding cmd to send */
			sqcp->state = CMD_STATE_IN_USE;
			ret = VTL_QUEUE_CMD;
			/* Can only send one header at a time */
			goto give_up;
		}
	}

give_up:
	return ret;
}

DECLARE_MUTEX(tmp_mutex);

static int vtl_remove_lu(int minor, char __user *arg)
{
	struct vtl_ctl ctl;
	struct vtl_hba_info *vtl_hba;
	struct vtl_lu_info *lu, *n;
	int ret = -ENODEV;

	down(&tmp_mutex);

	if (copy_from_user((u8 *)&ctl, (u8 *)arg, sizeof(ctl))) {
		ret = -EFAULT;
		goto give_up;
	}
	vtl_hba = vtl_get_hba_entry();
	printk("mhvtl: %s() ioctl to remove device <c t l> <%02d %02d %02d>, "
		"hba: %p\n", __func__, ctl.channel, ctl.id, ctl.lun, vtl_hba);

	list_for_each_entry_safe(lu, n, &vtl_hba->lu_list, lu_sibling) {
		if ((lu->channel == ctl.channel) && (lu->target == ctl.id) &&
						(lu->lun == ctl.lun)) {
			if (VTL_OPT_NOISE & vtl_opts)
				printk("mhvtl: %s line %d found matching lu\n",
					__func__, __LINE__);
			list_del(&lu->lu_sibling);
			devp[minor] = NULL;
			scsi_remove_device(lu->sdev);
			scsi_device_put(lu->sdev);
		}
	}

	ret = 0;

give_up:
	up(&tmp_mutex);
	return ret;
}

/*
 * char device ioctl entry point
 */
static int vtl_c_ioctl(struct inode *inode, struct file *file,
					unsigned int cmd, unsigned long arg)
{
	unsigned int minor = iminor(inode);
	int ret;

	if (minor > DEF_MAX_MINOR_NO) {	/* Check limit minor no. */
		return -ENODEV;
	}

	ret = 0;

	switch (cmd) {

	case VTL_POLL_AND_GET_HEADER:
		if (!devp[minor]) {
			put_user(0, (unsigned int *)arg);
			ret = 0;
			break;
		}
		ret = send_vtl_header(minor, (char __user *)arg);
		break;

	case VTL_GET_DATA:
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: ioctl(VTL_GET_DATA)\n");
		ret = get_user_data(minor, (char __user *)arg);
		break;

	case VTL_PUT_DATA:
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: ioctl(VTL_PUT_DATA)\n");
		ret = put_user_data(minor, (char __user *)arg);
		break;

	case VTL_REMOVE_LU:
		if (VTL_OPT_NOISE & vtl_opts)
			printk("mhvtl: ioctl(VTL_REMOVE_LU)\n");
		ret = vtl_remove_lu(minor, (char __user *)arg);
		break;

	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static int vtl_release(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);
	if (VTL_OPT_NOISE & vtl_opts)
		printk("mhvtl%d: Release\n", minor);
	return 0;
}

static int vtl_open(struct inode *inode, struct file *filp)
{
	unsigned int minor = iminor(inode);
	printk("mhvtl%d: opened\n", minor);
	return 0;
}

