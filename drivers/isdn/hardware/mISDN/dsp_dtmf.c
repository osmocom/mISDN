/* $Id: dsp_dtmf.c,v 1.1 2003/10/24 21:23:05 keil Exp $
 *
 * DTMF decoder.
 *
 * Copyright 2003       by Andreas Eversberg (jolly@jolly.de)
 *			based on different decoders such as ISDN4Linux
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 */
#warning KARSTEN: dein dtmf-modul benutzt eine andere bit-order, wieso geht das?

#include "layer1.h"
#include "helper.h"
#include "debug.h"
#include "dsp.h"

#define NCOEFF            8     /* number of frequencies to be analyzed */
#define DTMF_TRESH     200000L /* above this is dtmf (square of)*/

/* For DTMF recognition:
 * 2 * cos(2 * PI * k / N) precalculated for all k
 */
static signed long long cos2pik[NCOEFF] =
{
	/* k << 15 (source: hfc-4s document (www.colognechip.de)) */
	55960, 53912, 51402, 48438, 38146, 32650, 26170, 18630
};

/* digit matrix */
static char dtmf_matrix[4][4] =
{
	{'1', '2', '3', 'A'},
	{'4', '5', '6', 'B'},
	{'7', '8', '9', 'C'},
	{'*', '0', '#', 'D'}
};

/* dtmf detection using goertzel algorithm
 * init function
 */
void dsp_dtmf_goertzel_init(dsp_t *dsp)
{
	dsp->dtmf.size = 0;
	dsp->dtmf.lastwhat = '\0';
	dsp->dtmf.lastdigit = '\0';
	dsp->dtmf.count = 0;
}


/*************************************************************
 * calculate the coefficients of the given sample and decode *
 *************************************************************/

/* the given sample is decoded. if the sample is not long enough for a
 * complete frame, the decoding is finished and continued with the next
 * call of this function.
 *
 * the algorithm is very good for detection with a minimum of errors. i
 * tested it allot. it even works with very short tones (40ms). the only
 * disadvantage is, that it doesn't work good with different volumes of both
 * tones. this will happen, if accoustically coupled dialers are used.
 * it sometimes detects tones during speach, which is normal for decoders.
 * use sequences to given commands during calls.
 *
 * dtmf - points to a structure of the current dtmf state
 * spl and len - the sample
 * fmt - 0 = alaw, 1 = ulaw, 2 = coefficients from HFC DTMF hw-decoder
 */

unsigned char 
*dsp_dtmf_goertzel_decode(dsp_t *dsp, unsigned char *data, int len, int fmt)
{
	unsigned char what;
	int size;
	signed short *buf;
	signed long sk, sk1, sk2;
	int k, n, i;
	signed long result[NCOEFF], tresh, treshl;
	int lowgroup, highgroup;

	dsp->dtmf.digits[0] = '\0';

	/* note: the function will loop until the buffer are not enough samples
	 * left to decode a full frame
	 */
again:
	/* convert samples */
	size = dsp->dtmf.size;
	buf = dsp->dtmf.buffer;
	switch(fmt) {
		case 0: /* alaw */
		while(size<DSP_DTMF_NPOINTS && len) {
			buf[size++] = dsp_audio_alaw_to_s32[*data++];
			len--;
		}
		break;

		case 1: /* ulaw */
		while(size<DSP_DTMF_NPOINTS && len) {
			buf[size++] = dsp_audio_ulaw_to_s32[*data++];
			len--;
		}
		break;

		case 2: /* HFC coefficients */
		default:
		if (len == 0)
			return(dsp->dtmf.digits);
		if (len < sizeof(result)) {
			printk(KERN_ERR "%s: coefficients have invalid size.\n",
				__FUNCTION__);
			return(dsp->dtmf.digits);
		}
		memcpy(result, data, sizeof(result));
		data += sizeof(result);
		len -= sizeof(result);
		goto coefficients;
		break;
	}
	dsp->dtmf.size = size;

	if (size < DSP_DTMF_NPOINTS)
		return(dsp->dtmf.digits);

	dsp->dtmf.size = 0;

	/* now we have a full buffer of signed long samples - we do goertzel */
	for (k = 0; k < NCOEFF; k++) {
		sk = sk1 = sk2 = 0;
		buf = dsp->dtmf.buffer;
		for (n = 0; n < DSP_DTMF_NPOINTS; n++) {
			sk = ((cos2pik[k]*sk1)>>15) - sk2 + (*buf++);
			sk2 = sk1;
			sk1 = sk;
		}
		sk>>=8;
		sk2>>=8;
		if (sk>32767 || sk<-32767 || sk2>32767 || sk2<-32767)
			printk(KERN_WARNING "DTMF-Detection overflow\n");
		/* compute |X(k)|**2 */
		result[k] =
		   	 (sk * sk) -
		   	 (((cos2pik[k] * sk) >> 15) * sk2) +
		   	 (sk2 * sk2);
	}

	/* our (squared) coefficients have been calculated, we need to process
	 * them.
	 */
	coefficients:
	tresh = 0;
	for (i = 0; i < NCOEFF; i++) {
		if (result[i] < 0)
			result[i] = 0;
		if (result[i] > DTMF_TRESH) {
			if (result[i] > tresh)
				tresh = result[i];
		}
	}

	if (tresh == 0) {
		what = 0;
		goto storedigit;
	}

	if (dsp->debug & DEBUG_DSP_DTMFCOEFF)
		printk(KERN_DEBUG "a %3ld %3ld %3ld %3ld %3ld %3ld %3ld %3ld"
			" tr:%3ld r %3ld %3ld %3ld %3ld %3ld %3ld %3ld %3ld\n",
			result[0]/10000, result[1]/10000, result[2]/10000,
			result[3]/10000, result[4]/10000, result[5]/10000,
			result[6]/10000, result[7]/10000, tresh/10000,
			result[0]/(tresh/100), result[1]/(tresh/100),
			result[2]/(tresh/100), result[3]/(tresh/100),
			result[4]/(tresh/100), result[5]/(tresh/100),
			result[6]/(tresh/100), result[7]/(tresh/100));

	/* calc digit (lowgroup/highgroup) */
	lowgroup = highgroup = -1;
	treshl = tresh >> 3;  /* tones which are not on, must be below 9 dB */
	tresh = tresh >> 2;  /* touchtones must match within 6 dB */
	for (i = 0; i < NCOEFF; i++) {
		if (result[i] < treshl)
			continue;  /* ignore */
		if (result[i] < tresh) {
			lowgroup = highgroup = -1;
			break;  /* noise inbetween */
		}
		/* good level found. This is allowed only one time per group */
		if (i < NCOEFF/2) {
			/* lowgroup*/
			if (lowgroup >= 0) {
				// Bad. Another tone found. */
				lowgroup = -1;
				break;
			} else
				lowgroup = i;
		} else {
			/* higroup */
			if (highgroup >= 0) {
				// Bad. Another tone found. */
				highgroup = -1;
				break;
			} else
				highgroup = i-(NCOEFF/2);
		}
	}

	/* get digit or null */
	what = 0;
	if (lowgroup>=0 && highgroup>=0)
		what = dtmf_matrix[lowgroup][highgroup];

storedigit:
	if (what && (dsp->debug & DEBUG_DSP_DTMF))
		printk(KERN_DEBUG "DTMF what: %c\n", what);

	if (dsp->dtmf.lastwhat!=what)
		dsp->dtmf.count = 0;

	/* the tone (or no tone) must remain 3 times without change */
	if (dsp->dtmf.count == 2) {
		if (dsp->dtmf.lastdigit!=what) {
			dsp->dtmf.lastdigit = what;
			if (what) {
				if (dsp->debug & DEBUG_DSP_DTMF)
					printk(KERN_DEBUG "DTMF digit: %c\n",
						what);
				if ((strlen(dsp->dtmf.digits)+1) <sizeof(dsp->dtmf.digits)) {
					dsp->dtmf.digits[strlen(dsp->dtmf.digits)+1] = '\0';
					dsp->dtmf.digits[strlen(dsp->dtmf.digits)] = what;
				}
			}
		}
	} else
		dsp->dtmf.count++;

	dsp->dtmf.lastwhat = what;

	goto again;
}


