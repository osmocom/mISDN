/* $Id: dsp_cmx.c,v 1.1 2003/10/24 21:23:05 keil Exp $
 *
 * Linux ISDN subsystem, audio cmx (hardware level).
 *
 * Copyright 2002 by Andreas Eversberg (jolly@jolly.de)
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 */

/*
 * The process of adding and removing parties to/from a conference:
 *
 * There is a chain of conference_t which has one or more members in a chain
 * of conf_member_t.
 *
 * After a party is added, the conference is checked for hardware capability.
 * Also if a party is removed, the conference is checked again.
 *
 * There are 3 different solutions: -1 = software, 0 = hardware-crossconnect
 * 1-n = hardware-conference. The n will give the conference number.
 *
 * Depending on the change after removal or insertion of a party, hardware
 * commands are given.
 *
 * The current solution is stored within the conference_t entry.
 */

/* HOW THE CMX WORKS:
 *
 * There are 3 types of interaction: One member is alone, in this case only
 * data flow is done.
 * Two members will also exchange their data so they are crossconnected.
 * Three or more members will be added in a conference and will hear each
 * other but will not receive their own speech (echo) if not enabled.
 *
 * Features of CMX are:
 *  - Crossconnecting or even conference, if more than two members are together.
 *  - Force mixing of transmit data with other crossconnect/conference members.
 *  - Echo generation to benchmark the delay of audio processing.
 *  - Use hardware to minimize cpu load, disable FIFO load and minimize delay.
 *
 * There are 3 buffers:
 *
 * The conference buffer
 *
 *  R-3     R-2   R-1    W-2    W-1 W-3
 *   |       |     |      |      |   |
 * --+-------+-----+------+------+---+---------------
 *                        |          |
 *                      W-min      W-max
 *
 * The conference buffer is a ring buffer used to mix all data from all members.
 * To compensate the echo, data of individual member will later be substracted
 * before it is sent to that member. Each conference has a W-min and a W-max
 * pointer. Each individual member has a write pointer (W-x) and a read pointer
 * (R-x). W-min shows the lowest value of all W-x. The W-max shows the highest
 * value of all W-x. Whenever data is written, it is mixed by adding to the
 * existing sample value in the buffer. If W-max would increase, the additional
 * range is cleared so old data will be erased in the ring buffer.
 *
 *
 * RX-Buffer
 *                R-1           W-1
 *                 |             |
 * ----------------+-------------+-------------------
 * 
 * The rx-buffer is a ring buffer used to store the received data for each
 * individual member. To compensate echo, this data will later be substracted
 * from the conference's data before it is sent to that member. If only two
 * members are in one conference, this data is used to get the queued data from
 * the other member.
 *
 *
 * TX-Buffer
 *                  R        W
 *                  |        |
 * -----------------+--------+-----------------------
 *
 * The tx-buffer is a ring buffer to queue the transmit data from user space
 * until it will be mixed or sent. There are two pointers, R and W. If the write
 * pointer W would reach or overrun R, the buffer would overrun. In this case
 * (some) data is dropped so that it will not overrun.
 *
 *
 * If a member joins a conference:
 *
 * - If a member joins, its rx_buff is set to silence.
 * - If the conference reaches three members, the conf-buffer is cleared.
 * - When a member is joined, it will set its write and read pointer to W_max.
 *
 * The procedure of received data from card is explained in cmx_receive.
 * The procedure of received data from user space is explained in cmx_transmit.
 *
 *
 * LIMITS:
 *
 * The max_queue value is 2* the samples of largest packet ever received by any
 * conference member from her card. It also changes during life of conference.
 *
 *
 * AUDIO PROCESS:
 *
 * Writing data to conference's and member's buffer is done by adding the sample
 * value to the existing ring buffer. Writing user space data to the member's
 * buffer is done by substracting the sample value from the existing ring
 * buffer.
 *
 *
 * Interaction with other features:
 *
 * DTMF:
 * DTMF decoding is done before the data is crossconnected.
 *
 * Volume change:
 * Changing rx-volume is done before the data is crossconnected. The tx-volume
 * must be changed whenever data is transmitted to the card by the cmx.
 *
 * Tones:
 * If a tone is enabled, it will be processed whenever data is transmitted to
 * the card. It will replace the tx-data from the user space.
 * If tones are generated by hardware, this conference member is removed for
 * this time.
 *
 * Disable rx-data:
 * If cmx is realized in hardware, rx data will be disabled if requested by
 * the upper layer. If dtmf decoding is done by software and enabled, rx data
 * will not be diabled but blocked to the upper layer.
 *
 * HFC conference engine:
 * If it is possible to realize all features using hardware, hardware will be
 * used if not forbidden by control command. Disabling rx-data provides
 * absolutely traffic free audio processing. (except for the quick 1-frame
 * upload of a tone loop, only once for a new tone)
 *
 */

#include <linux/vmalloc.h>
#include "layer1.h"
#include "helper.h"
#include "debug.h"
#include "dsp.h"

//#define CMX_DEBUG /* massive read/write pointer output */

conference_t *Conf_list = NULL;

#warning remove debug "1" "2" u.s.w

/*
 * debug cmx memory structure
 */
void
dsp_cmx_debug(dsp_t *dsp)
{
	conference_t *conf;
	conf_member_t *member;
	dsp_t *odsp;

	printk(KERN_DEBUG "-----Current DSP\n");
	odsp = (dsp_t *)dsp_obj.ilist;
	while(odsp)
	{
		printk(KERN_DEBUG "* DSP 0x%lx echo=%d ulaw=%d txmix=%d", (unsigned long)odsp, odsp->echo, odsp->ulaw, odsp->tx_mix);
		if (odsp->conf)
			printk(" (Conf %d)", odsp->conf->id);
		if (dsp == odsp)
			printk(" *this*");
		printk("\n");
		odsp = odsp->next;
	}
	
	printk(KERN_DEBUG "-----Current Conf:\n");
	conf = Conf_list;
	while(conf)
	{
		printk(KERN_DEBUG "* Conf %d (0x%lx) solution=%d\n", conf->id, (unsigned long)conf, conf->solution);
		member = conf->mlist;
		while(member)
		{
			printk(KERN_DEBUG "  - member = 0x%lx(dsp)%s\n", (unsigned long)member->dsp, (member->dsp==dsp)?" *this*":"");
			member = member->next;
		}
		conf = conf->next;
	}
	printk(KERN_DEBUG "-----end\n");
}

/*
 * search conference
 */
static conference_t 
*dsp_cmx_search_conf(unsigned long id)
{
	conference_t *conf;

	if (!id) {
		printk(KERN_WARNING "%s: conference ID is 0.\n", 
			__FUNCTION__);
		return(NULL);
	}

	/* search conference */
	conf = Conf_list;
	while(conf) {
		if (conf->id == id)
			return(conf);
		conf = conf->next;
	}

	return(NULL);
}


/*
 * add member to conference
 */
static int
dsp_cmx_add_conf_member(dsp_t *dsp, conference_t *conf)
{
	conf_member_t *member;
	unsigned char zero;

printk(KERN_DEBUG "x\n");
	if (!conf || !dsp) {
		printk(KERN_WARNING "%s: conf or dsp is 0.\n", __FUNCTION__);
		return(-EINVAL);
	}
	if (dsp->member) {
		printk(KERN_WARNING "%s: dsp is already member in a conf.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	if (dsp->conf) {
		printk(KERN_WARNING "%s: dsp is already in a conf.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

printk(KERN_DEBUG "xx\n");
	if (!(member = vmalloc(sizeof(conf_member_t)))) {
		printk(KERN_ERR "vmalloc conf_member_t failed\n");
		return(-ENOMEM);
	}
printk(KERN_DEBUG "xxx\n");
	memset(member, 0, sizeof(conf_member_t));
	zero = (dsp->ulaw)?ulawsilence:alawsilence;
	memset(dsp->rx_buff, zero, sizeof(dsp->rx_buff));
	member->dsp = dsp;
	/* set initial values */
	dsp->W_rx = conf->W_max;
	dsp->R_rx = conf->W_max;

printk(KERN_DEBUG "xxxx\n");
	APPEND_TO_LIST(member, ((conf_member_t *)conf->mlist));

	/* zero conf-buffer if we change from 2 to 3 members */
	if (conf->mlist->next) if (!conf->mlist->next->next)
		memset(conf->conf_buff, 0, sizeof(conf->conf_buff));

printk(KERN_DEBUG "xxxxx\n");
	dsp->conf = conf;
	dsp->member = member;

	return(0);
}


/*
 * del member from conference
 */
int
dsp_cmx_del_conf_member(dsp_t *dsp)
{
	conf_member_t *member;

	if (!dsp) {
		printk(KERN_WARNING "%s: dsp is 0.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	if (!dsp->conf) {
		printk(KERN_WARNING "%s: dsp is not in a conf.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	member = dsp->conf->mlist;
	if (!member) {
		printk(KERN_WARNING "%s: dsp has linked an empty conf.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	/* find us in conf */
	while(member) {
		if (member->dsp == dsp)
			break;
		member = member->next;
	}
	if (!member) {
		printk(KERN_WARNING "%s: dsp is not present in its own conf_meber list.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	REMOVE_FROM_LISTBASE(member, ((conf_member_t *)dsp->conf->mlist));
	vfree(member);
	dsp->conf = NULL;
	dsp->member = NULL;

	return(0);
}


/*
 * new conference
 */
static conference_t
*dsp_cmx_new_conf(unsigned long id)
{
	conference_t *conf;

	if (!id) {
		printk(KERN_WARNING "%s: id is 0.\n", 
			__FUNCTION__);
		return(NULL);
	}

	if (!(conf = vmalloc(sizeof(conference_t)))) {
		printk(KERN_ERR "vmalloc conference_t failed\n");
		return(NULL);
	}
	memset(conf, 0, sizeof(conference_t));
	conf->id = id;
	conf->solution = -1;

	APPEND_TO_LIST(conf, ((conference_t *)Conf_list));

	return(conf);
}


/*
 * del conference
 */
int
dsp_cmx_del_conf(conference_t *conf)
{
	if (!conf) {
		printk(KERN_WARNING "%s: conf is null.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	if (conf->mlist) {
		printk(KERN_WARNING "%s: conf not empty.\n", 
			__FUNCTION__);
		return(-EINVAL);
	}

	REMOVE_FROM_LISTBASE(conf, ((conference_t *)Conf_list));
	vfree(conf);

	return(0);
}


/*
 * check if conference members are on an HFC-multi hardware
 *
 * analyse the members and check if they can be realized in hardware
 * also check if we have a conference available, which is limited to 8
 * dsp - one memeber (our own)
 * connect - the conference ID
 *
 * returns
 *	-1 = not possible or only one member, no need for hardware
 *	0 = only two members, we can do it without restrictions
 *	1-8 = more than three members, the given conference number is available
 */
static int 
dsp_cmx_hfc(conference_t *conf, int debug)
{
	conf_member_t *member;
	conference_t *conf_temp;
	int memb = 0, unit = 0, i;
	int freeunits[8] = {0,0,0,0,0,0,0,0};

	if(!conf)
		return(-1);

	member = conf->mlist;
	/* check all members in our conference */
	while(member) {
		/* check if member uses mixing */
		if (member->dsp->tx_mix)
			return(-1);
#ifdef WITH_HARDWARE
		/* check if member changes volume at an not suppoted level */
			if (member->dsp.tx_volume )
to be done: welche lautstärken werden vom hfc unterstützt
			if (member->dsp.rx_volume )
		}
#endif
		/* check if member is not on an HFC based card */
		if (!member->dsp->hfc_id)
			return(-1);
		/* check if relations are not on the same card */
		if (member->dsp->hfc_id != conf->mlist->dsp->hfc_id)
			return(-1);
			
		member = member->next;
		memb++;
	}

	/* if we have less than two members, we don't need any hardware */
	if (memb < 2)
		return(-1);

	/* ok, now we are sure that all members are on the same card.
	 * now we will see if we have only two members, so we can do
	 * crossconnections, which don't have any limitations.
	 */

	/* if we have only two members, we return 0 */
	if (memb == 2)
		return(0);

	/* if we have more than two, we may check if we have a conference
	 * unit available on the chip.
	 */
	conf_temp = Conf_list;
	while(conf_temp) {
		if (conf_temp != conf) { /* check only other conferences */
			if (conf->hfc_id == conf_temp->hfc_id) {
				if (conf_temp->solution > 8) {
					printk(KERN_WARNING "%s: unit(%d) of conference %d out of range.\n", __FUNCTION__, conf_temp->solution, conf_temp->id);
					return(-1);
				}
				if (conf_temp->solution > 0) {
					if (freeunits[conf_temp->solution-1]) {
						printk(KERN_WARNING "%s: unit(%d) of conference %d was used by another conference(%d).\n", __FUNCTION__, conf_temp->solution, conf_temp->id, freeunits[conf_temp->solution-1]);
						return(-1);
					}
					freeunits[conf_temp->solution-1] = conf_temp->id;
					unit++;
				}
			}
		}
		conf_temp = conf_temp->next;
	}
	if (debug & DEBUG_DSP_CMX)
		printk(KERN_DEBUG "%s: currently there are %d other conf units in use.\n", __FUNCTION__, unit);
	if (unit > 8) {
		printk(KERN_WARNING "%s: too many units(%d) in use.\n", __FUNCTION__,  unit);
		return(-1);
	}
	/* return the current unit, if available */
	if (conf->solution > 0)
		return(conf->solution);
	/* return the free unit number */
	i = 0;
	while(i < 8)
	if (unit > 8) {
		if (freeunits[i] == 0)
			return(i+1);
		i++;
	}

	return(-1);
}


/*
 * send crossconnect message to hfc card
 */
void
dsp_cmx_hfc_cross_message(dsp_t *dsp1, dsp_t *dsp2, int enable)
{
#ifdef WITH_HARDWARE
	to be done!
#endif
}

void
dsp_cmx_hfc_conf_message(dsp_t *dsp, int num)
{
#ifdef WITH_HARDWARE
	to be done!
#endif
}


/*
 * add or remove party to the conference and check hardware capability
 *
 *
 * the cmx will handle audio data via software process. if it is possible
 * to do it in hardware, the hardware feature is used.
 *
 * dsp - the layer object
 * connect - conference ID to connect to (0 = remove from any conference)
 */
int
dsp_cmx(dsp_t *dsp)
{
	int err;
	int old, new;
	conf_member_t *member, *member1, *member2;
	conference_t *conf;

	if (dsp->debug & DEBUG_DSP_CMX)
		printk(KERN_DEBUG "dsp_cmx called with conf_id=%d\n",
				dsp->conf_id);

	/* check if we are not in a conf */
	if (!dsp->conf) {
		if (!dsp->conf_id)
			return(0);
		goto add_to_conf;
	}

	if (dsp->conf_id && dsp->b_active) {
		/* if conference changes, we need to remove first */
		if (dsp->conf_id != dsp->conf->id)
			goto remove_from_conf;
		else
			return(0); /* nothing changed */
	}

	/* see if we get removed from a conference */
	if (!dsp->conf_id || !dsp->b_active) {
		remove_from_conf:
		if (dsp->debug & DEBUG_DSP_CMX)
			printk(KERN_DEBUG "removing us from conference %d\n",
				dsp->conf->id);
		conf = dsp->conf;
		old = conf->solution;
		member1 = NULL;
		member2 = NULL;
		if (conf->mlist)
		if (conf->mlist->next)
		if (!conf->mlist->next->next) {
			member1 = conf->mlist;
			member2 = conf->mlist->next;
		}
		/* remove member from conference */
		err = dsp_cmx_del_conf_member(dsp);
		if (err)
			return(err);
		new = dsp_cmx_hfc(conf, dsp->debug);
		/* check changes in hardware settings */
		if (new>0 && old>0)
		{
			/* only we will get removed */
			dsp_cmx_hfc_conf_message(dsp, 0);
		}
		if (new<=0 && old>0) {
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw conference not needed anymore, so we remove it.\n");
			/* conferrence is not needed anymore */
			member = conf->mlist;
			while(member) {
				dsp_cmx_hfc_conf_message(member->dsp, 0);
				member = member->next;
			}
			conf->solution = -1; /* software from now on */
			conf->hfc_id = 0; /* conf is not supported by a hfc chip */
		}
		if (new<0 && old==0) {
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw crossconnect not needed anymore, so we remove it.\n");
			/* crossconnect is not needed anymore */
			if (!member1 || !member2) {
				only2members:
				printk(KERN_ERR "%s: fatal error. expecting exactly two crossconnected members.\n", __FUNCTION__);
				return(-EINVAL);
			}
			dsp_cmx_hfc_cross_message(member1->dsp, member2->dsp, 0);
			conf->solution = -1; /* software from now on */
			conf->hfc_id = 0; /* conf is not supported by a hfc chip */
		}
		if (new==0) {
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw crossconnect has become possible.\n");
			/* crossconnect is now possible */
			member = conf->mlist;
			if (!member)
				goto only2members;
			if (!member->next)
				goto only2members;
			if (member->next->next)
				goto only2members;
			dsp_cmx_hfc_cross_message(member->dsp, member->next->dsp, 1);
			conf->solution = 0; /* hard crossconnect from now on */
			conf->hfc_id = dsp->hfc_id; /*cross is supported by hfc chip*/
		}

		if (!conf->mlist)
		{
			err = dsp_cmx_del_conf(conf);
			if (err)
				return(err);
		}
	}

add_to_conf:
	/* see if we get connected to a conference */
	if (dsp->conf_id && dsp->b_active) {
		if (dsp->debug & DEBUG_DSP_CMX)
			printk(KERN_DEBUG "searching conference %d\n",
				dsp->conf_id);
		conf = dsp_cmx_search_conf(dsp->conf_id);
		if (!conf)
		{
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "conference doesn't exist yet, creating.\n");
			/* the conference doesn't exist, so we create */
printk(KERN_DEBUG "1\n");
			conf = dsp_cmx_new_conf(dsp->conf_id);
printk(KERN_DEBUG "2\n");
			if (!conf)
				return(-EINVAL);
		}
printk(KERN_DEBUG "3\n");
		member1 = NULL;
		member2 = NULL;
		if (conf->mlist)
		if (conf->mlist->next)
		if (!conf->mlist->next->next) {
printk(KERN_DEBUG "4\n");
			member1 = conf->mlist;
			member2 = conf->mlist->next;
		}
		/* add conference member */
printk(KERN_DEBUG "5\n");
		err = dsp_cmx_add_conf_member(dsp, conf);
printk(KERN_DEBUG "6\n");
		if (err)
			return(err);

		/* if we are alone, we do nothing! */
		if (!conf->next) {
printk(KERN_DEBUG "7\n");
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "we are alone in this conference, so exit.\n");
			return(0);
		}
printk(KERN_DEBUG "8\n");

		/* check changes in hardware settings */
		new = dsp_cmx_hfc(conf, dsp->debug);
		old = conf->solution;
		if (new<=0 && old>0) {
printk(KERN_DEBUG "81\n");
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw conference has become too complex, so we remove it.\n");
			/* conferrence will get removed due to complexity of conference */
			member = conf->mlist;
			while(member) {
				dsp_cmx_hfc_conf_message(member->dsp, 0);
				member = member->next;
			}
			conf->solution = -1; /* software from now on */
			conf->hfc_id = 0; /* conf is not supported by a hfc chip */
		}
printk(KERN_DEBUG "9\n");
		if (new!=0 && old==0) {
printk(KERN_DEBUG "91\n");
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw crossconnect has become too complex, so we remove simple crossconnect.\n");
			/* crossconnect will get removed due to complexity of conference */
			if (!member1 || !member2)
				goto only2members;
			dsp_cmx_hfc_cross_message(member1->dsp, member2->dsp, 0);
			conf->solution = -1; /* software from now on */
			conf->hfc_id = 0; /* conf is not supported by a hfc chip */
		}
printk(KERN_DEBUG "a\n");
		if (new>0) {
printk(KERN_DEBUG "a1\n");
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw conference becomes possible.\n");
			/* conference is now possible */
			member = conf->mlist;
			while(member) {
				dsp_cmx_hfc_conf_message(member->dsp, new);
				member = member->next;
			}
			conf->solution = new; /* conference from now on */
			conf->hfc_id = dsp->hfc_id; /*conf is supported by hfc chip*/
		}
printk(KERN_DEBUG "b\n");
		if (new==0) {
printk(KERN_DEBUG "b1\n");
			if (dsp->debug & DEBUG_DSP_CMX)
				printk(KERN_DEBUG "hw crossconnect has become possible.\n");
			/* crossconnect is now possible */
			member = conf->mlist;
			if (!member)
				goto only2members;
			if (!member->next)
				goto only2members;
			if (member->next->next)
				goto only2members;
			dsp_cmx_hfc_cross_message(member->dsp, member->next->dsp, 1);
			conf->solution = 0; /* hard crossconnect from now on */
			conf->hfc_id = dsp->hfc_id; /*cross is supported by hfc chip*/
		}
printk(KERN_DEBUG "c\n");
	}

	return(0);
}


/*
 * audio data is received from card
 */

void 
dsp_cmx_receive(dsp_t *dsp, struct sk_buff *skb)
{
	conference_t *conf = dsp->conf;
	conf_member_t *member;
	signed long *c;
	unsigned char *d, *p;
	int len = skb->len;
	int w, ww, i, ii;
	int W_min, W_max;
	signed long *decode_law;

	decode_law = (dsp->ulaw)?dsp_audio_ulaw_to_s32:dsp_audio_alaw_to_s32;

	/* check if we have sompen */
	if (len < 1)
		return;

	/* -> if length*2 is greater largest */
	if (dsp->largest < (len<<1))
		dsp->largest = (len<<1);

	/* half of the buffer should be 4 time larger than maximum packet size */
	if (len >= CMX_BUFF_HALF>>2) {
		printk(KERN_ERR "%s line %d: packet from card is too large (%d bytes). please make card send smaller packets OR increase CMX_BUFF_SIZE\n", __FILE__, __LINE__, len);
		return;
	}

	/* STEP 1: WRITE DOWN WHAT WE GOT (into the buffer(s) */

	/* -> new W-min is calculated:
	 * W_min will be the write pointer of this dsp (after writing 'len'
	 * of bytes).
	 * If there are other members in a conference, W_min will be the
	 * lowest of all member's writer pointers.
	 */
	W_min = (dsp->W_rx + len) & CMX_BUFF_MASK;
	if (conf) {
		/* -> who is larger? dsp or conf */
		if (conf->largest < dsp->largest)
			conf->largest = dsp->largest;
		if (conf->largest > dsp->largest)
			dsp->largest = conf->largest;

		member = conf->mlist;
		while(member) {
			if (member != dsp->member)
				/* if W_rx is lower */
				if (((member->dsp->W_rx - W_min) & CMX_BUFF_MASK) >= CMX_BUFF_HALF)
					W_min = member->dsp->W_rx;
			member = member->next;
		}
		/* store for dsp_cmx_send */
		conf->W_min = W_min;
	}
	/* -> new W-max is calculated:
	 * W_max will be the highest write pointer in the conference.
	 */
	W_max = W_min;
	if (conf) {
		/* if conf->W_max is higher */
		if (((W_max - conf->W_max) & CMX_BUFF_MASK) >= CMX_BUFF_HALF)
			W_max = conf->W_max;
//		W_max = conf->W_max;
//		/* if W_rx+len is higher */
//		if (((W_max - dsp->W_rx - len) & CMX_BUFF_MASK) >= CMX_BUFF_HALF)
//			W_max = (dsp->W_rx + len) & CMX_BUFF_MASK;
	}

#ifdef CMX_DEBUG
	printk(KERN_DEBUG "cmx_receive(dsp=%lx): W_rx(dsp)=%05x W_min=%05x W_max=%05x largest=%05x\n", dsp, dsp->W_rx, W_min, W_max, dsp->largest);
#endif

	/* -> if data is not too fast (exceed maximum queue):
	 * data is written if W_max is not too high (largest).
	 * W_max will be increased if we are the fastest writer, but if we
	 * are not too far off the slowest member, which is W_min.
	 * if we are alone, we are always the fastest and the slowest, so we
	 * always write.
	 */
	if (((W_max - W_min) & CMX_BUFF_MASK) <= dsp->largest) {
		/* -> received data is written to rx-buffer */
		p = skb->data;
		d = dsp->rx_buff;
		w = dsp->W_rx;
		i = 0;
		ii = len;
		while(i < ii) {
			d[w++ & CMX_BUFF_MASK] = *p++;
			i++;
		}
		/* -> if conference has three or more members */
		if (conf) if (conf->next) if (conf->next->next) {
			/* -> received data is added to conf-buffer
			 *    new space is overwritten */
			p = skb->data;
			c = conf->conf_buff;
			w = dsp->W_rx;
			ww = conf->W_max;
			i = 0;
			ii = len;
			/* loop until done or old W_max is reached */
			while(i<ii && w!=ww) {
				c[w] += decode_law[*p++]; /* add to existing */
				w = (w+1) & CMX_BUFF_MASK;
				i++;
			}
			/* loop the rest */
			while(i < ii) {
				c[w] = decode_law[*p++]; /* write to new */
				w = (w+1) & CMX_BUFF_MASK;
				i++;
			}
		}
		/* -> write new W_max and W_rx */
		if (conf)
			conf->W_max = W_max;
		dsp->W_rx = (dsp->W_rx + len) & CMX_BUFF_MASK;
	} else {
		if (dsp->debug & DEBUG_DSP_CMX)
			printk(KERN_DEBUG "receiving too fast (rx_buff).\n");
	}
}

/*
 * send mixed audio data to card
 */

struct sk_buff 
*dsp_cmx_send(dsp_t *dsp, int len, int dinfo)
{
	conference_t *conf = dsp->conf;
	dsp_t *member, *other;
	register signed long sample;
	signed long *c;
	unsigned char *d, *o, *p, *q;
	struct sk_buff *nskb;
	int r, rr, t, tt;
	unsigned char *encode_law, zero;
	signed long *decode_law, *odecode_law;
//	unsigned short *mix_law;

	decode_law = (dsp->ulaw)?dsp_audio_ulaw_to_s32:dsp_audio_alaw_to_s32;
	encode_law = (dsp->ulaw)?dsp_audio_s16_to_ulaw:dsp_audio_s16_to_alaw;
//	mix_law = (dsp->ulaw)?dsp_audio_s16_to_ulaw:dsp_audio_s16_to_alaw;
	zero = (dsp->ulaw)?ulawsilence:alawsilence;

	/* PREPARE RESULT */
//printk("a");
	nskb = alloc_skb(len, GFP_ATOMIC);
	if (!nskb) {
		printk(KERN_ERR "FATAL ERROR in mISDN_dsp.o: cannot alloc %d bytes\n", len);
		return(NULL);
	}
	mISDN_sethead(PH_DATA | REQUEST, dinfo, nskb);
	/* set pointers, indexes and stuff */
	member = dsp;
	p = dsp->tx_buff; /* transmit data */
	q = dsp->rx_buff; /* received data */
	d = skb_put(nskb, len); /* result */
	t = dsp->R_tx; /* tx-pointers */
	tt = dsp->W_tx;
	r = dsp->R_rx; /* rx-pointers */
	if (conf)
		/* W_min is also limit for read */
		rr = conf->W_min;
	else
		rr = dsp->W_rx;
	/* calculate actual r (if r+len would overrun rr) */
	if (((rr - r - len) & CMX_BUFF_MASK) >= CMX_BUFF_HALF)
		/* r is set "len" bytes before W_min */
		r = (rr - len) & CMX_BUFF_MASK;
	else
		/* rr is set "len" bytes after R_rx */
		rr = (r + len) & CMX_BUFF_MASK;
	dsp->R_rx = rr;
	/* now: rr is exactly "len" bytes after r now */
#ifdef CMX_DEBUG
	printk(KERN_DEBUG "CMX_SEND(dsp=%lx) %d bytes from tx:0x%05x-0x%05x rx:0x%05x-0x%05x echo=%d\n", dsp, len, t, tt, r, rr, dsp->echo);
#endif
//printk("b");

	/* STEP 2.0: PROCESS TONES/TX-DATA ONLY */
	if (dsp->tone.tone) {
		/* -> copy tone */
		dsp_tone_copy(dsp, d, len);
		dsp->R_tx = dsp->W_tx = 0; /* clear tx buffer */
		return(nskb);
	}
	/* if we have tx-data but do not use mixing */
	if (!dsp->tx_mix && t!=tt) {
		/* -> send tx-data and continue when not enough */
		while(r!=rr && t!=tt) {
			*d++ = p[t]; /* write tx_buff */
			t = (t+1) & CMX_BUFF_MASK;
			r = (r+1) & CMX_BUFF_MASK;
		}
		if(r == rr) {
			dsp->R_tx = t;
			return(nskb);
		}
	}
	/* STEP 2.1: PROCESS DATA (one member / no conf) */
//printk("c");
	if (!conf) {
//printk("d");
		single:
		/* -> if echo is NOT enabled */
		if (!dsp->echo) {
			/* -> send tx-data if available or use 0-volume */
			while(r!=rr && t!=tt) {
				*d++ = p[t]; /* write tx_buff */
				t = (t+1) & CMX_BUFF_MASK;
				r = (r+1) & CMX_BUFF_MASK;
			}
			if(r != rr)
				memset(d, zero, (rr-r)&CMX_BUFF_MASK);
		/* -> if echo is enabled */
		} else {
//printk(KERN_DEBUG "echo krach:%x %x %x %x ulaw=%d\n", r, rr, t, tt,dsp->ulaw);
			/* -> mix tx-data with echo if available, or use echo only */
			while(r!=rr && t!=tt) {
#warning miximg can be optimized using *d++ = alaw_mix[(p[t]<<8)|q[r]]
				sample = decode_law[p[t]] + decode_law[q[r]];
				if (sample < -32768)
					sample = -32768;
				else if (sample > 32767)
					sample = 32767;
				*d++ = encode_law[sample & 0xffff]; /* tx-data + echo */
				t = (t+1) & CMX_BUFF_MASK;
				r = (r+1) & CMX_BUFF_MASK;
			}
			while(r != rr) {
				*d++ = q[r]; /* echo */
				r = (r+1) & CMX_BUFF_MASK;
			}
		}
		dsp->R_tx = t;
		return(nskb);
	}
	if (!conf->mlist->next) {
//printk(".");
		goto single;
	}
//printk("e");
	/* STEP 2.2: PROCESS DATA (two members) */
	if (!conf->mlist->next->next) {
//printk("f");
		/* "other" becomes other party */
		other = conf->mlist->dsp;
		if (other == member)
			other = conf->mlist->next->dsp;
		o = other->rx_buff; /* received data */
		odecode_law = (other->ulaw)?dsp_audio_ulaw_to_s32:dsp_audio_alaw_to_s32;
//printk("+");
		/* -> if echo is NOT enabled */
		if (!dsp->echo) {
//printk("-");
			/* -> copy other member's rx-data, if tx-data is available, mix */
			while(r!=rr && t!=tt) {
				sample = decode_law[p[t]] + odecode_law[o[r]];
				if (sample < -32768)
					sample = -32768;
				else if (sample > 32767)
					sample = 32767;
				*d++ = encode_law[sample & 0xffff]; /* tx-data + echo */
				t = (t+1) & CMX_BUFF_MASK;
				r = (r+1) & CMX_BUFF_MASK;
			}
			while(r != rr) {
				*d++ = o[r];
				r = (r+1) & CMX_BUFF_MASK;
			}
		/* -> if echo is enabled */
		} else {
//printk("*");
			/* -> mix other member's rx-data with echo, if tx-data is available, mix */
			while(r!=rr && t!=tt) {
				sample = decode_law[p[t]] + odecode_law[o[r]] + decode_law[q[r]];
				if (sample < -32768)
					sample = -32768;
				else if (sample > 32767)
					sample = 32767;
				*d++ = encode_law[sample & 0xffff]; /* tx-data + rx_data + echo */
				t = (t+1) & CMX_BUFF_MASK;
				r = (r+1) & CMX_BUFF_MASK;
			}
//printk("/");
			while(r != rr) {
				sample = odecode_law[o[r]] + decode_law[q[r]];
				if (sample < -32768)
					sample = -32768;
				else if (sample > 32767)
					sample = 32767;
				*d++ = encode_law[sample & 0xffff]; /* rx-data + echo */
				r = (r+1) & CMX_BUFF_MASK;
			}
		}
//printk("&");
		dsp->R_tx = t;
		return(nskb);
	}
//printk("g");
	/* STEP 2.3: PROCESS DATA (three or more members) */
	c = conf->conf_buff;
	/* -> if echo is NOT enabled */
	if (!dsp->echo) {
		/* -> substract rx-data from conf-data, if tx-data is available, mix */
		while(r!=rr && t!=tt) {
			sample = decode_law[p[t]] + c[r] - decode_law[q[r]];
			if (sample < -32768)
				sample = -32768;
			else if (sample > 32767)
				sample = 32767;
			*d++ = encode_law[sample & 0xffff]; /* conf-rx+tx */
			t = (t+1) & CMX_BUFF_MASK;
			r = (r+1) & CMX_BUFF_MASK;
		}
		while(r != rr) {
			sample = c[r] - decode_law[q[r]];
			if (sample < -32768)
				sample = -32768;
			else if (sample > 32767)
				sample = 32767;
			*d++ = encode_law[sample & 0xffff]; /* conf-rx */
			r = (r+1) & CMX_BUFF_MASK;
		}
	/* -> if echo is enabled */
	} else {
		/* -> encode conf-data, if tx-data is available, mix */
		while(r!=rr && t!=tt) {
			sample = decode_law[p[t]] + c[r];
			if (sample < -32768)
				sample = -32768;
			else if (sample > 32767)
				sample = 32767;
			*d++ = encode_law[sample & 0xffff]; /* conf(echo)+tx */
			t = (t+1) & CMX_BUFF_MASK;
			r = (r+1) & CMX_BUFF_MASK;
		}
		while(r != rr) {
			sample = c[r];
			if (sample < -32768)
				sample = -32768;
			else if (sample > 32767)
				sample = 32767;
			*d++ = encode_law[sample & 0xffff]; /* conf(echo) */
			r = (r+1) & CMX_BUFF_MASK;
		}
	}
	dsp->R_tx = t;
	return(nskb);
}

/*
 * audio data is transmitted from upper layer to the dsp
 */
void 
dsp_cmx_transmit(dsp_t *dsp, struct sk_buff *skb)
{
	u_int w, ww;
	unsigned char *d, *p;
	int space;

	/* check if we have sompen */
	if (skb->len < 1)
		return;

	/* check if there is enough space, and then copy */
	p = dsp->tx_buff;
	d = skb->data;
	w = dsp->W_tx;
	ww = dsp->R_tx;
	space = ww-w;
	if (space <= 0)
		space += CMX_BUFF_SIZE;
	/* write-pointer should not overrun nor reach read pointer */
	if (space-1 < skb->len)
		/* write to the space we have left */
		ww = (ww - 1) & CMX_BUFF_MASK;
	else
		/* write until all byte are copied */
		ww = (w + skb->len) & CMX_BUFF_MASK;
	dsp->W_tx = ww;

#ifdef CMX_DEBUG
	printk(KERN_DEBUG "cmx_transmit(dsp=%lx) %d bytes to 0x%x-0x%x.\n", dsp, (ww-w)&CMX_BUFF_MASK, w, ww);
#endif

	/* copy transmit data to tx-buffer */
	while(w != ww) {
		p[w]= *d++;
		w = (w+1) & CMX_BUFF_MASK;
	}

	return;
}


