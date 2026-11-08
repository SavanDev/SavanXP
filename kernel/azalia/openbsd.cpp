/*-
 * Copyright (c) 2005 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by TAMURA Kent
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// Selective OpenBSD port. Upstream routines below keep their names and flow.
// Local changes: bus callbacks, self in place of C's this, polled RIRB status,
// no unsolicited queue, response codec validation and one converter per stream.
// See AZALIA.md.
#include "azalia_port.hpp"

namespace azalia_port {
namespace {
constexpr int ETIMEDOUT = 110, EINVAL = 22, EIO = 5;
#define DPRINTF(x) do {} while (0)
#define DPRINTFN(n,x) do {} while (0)
using bus_addr_t = uint64_t;
#define XNAME(x) "azalia"
#define printf(...) do {} while (0)
#define PTR_UPPER32(x) (static_cast<uint64_t>(x) >> 32)
#define AZALIA_DMA_DMAADDR(p) ((p)->physical)
#define AZ_READ_1(a,r) ((a)->io.read((a)->io.context, HDA_##r, 1))
#define AZ_READ_2(a,r) ((a)->io.read((a)->io.context, HDA_##r, 2))
#define AZ_READ_4(a,r) ((a)->io.read((a)->io.context, HDA_##r, 4))
#define AZ_WRITE_1(a,r,v) ((a)->io.write((a)->io.context, HDA_##r, (v), 1))
#define AZ_WRITE_2(a,r,v) ((a)->io.write((a)->io.context, HDA_##r, (v), 2))
#define AZ_WRITE_4(a,r,v) ((a)->io.write((a)->io.context, HDA_##r, (v), 4))
#define STR_READ_1(s,r) ((s)->az->io.read((s)->az->io.context, (s)->regbase + HDA_SD_##r, 1))
#define STR_READ_2(s,r) ((s)->az->io.read((s)->az->io.context, (s)->regbase + HDA_SD_##r, 2))
#define STR_WRITE_1(s,r,v) ((s)->az->io.write((s)->az->io.context, (s)->regbase + HDA_SD_##r, (v), 1))
#define STR_WRITE_2(s,r,v) ((s)->az->io.write((s)->az->io.context, (s)->regbase + HDA_SD_##r, (v), 2))
#define STR_WRITE_4(s,r,v) ((s)->az->io.write((s)->az->io.context, (s)->regbase + HDA_SD_##r, (v), 4))
#define VALID_WIDGET_NID(n,c) ((n) >= (c)->wstart && (n) < (c)->wend)
#define AZ_QRK_WID_OVREF50 0x00004000
} // namespace

// From sys/dev/pci/azalia.c: azalia_reset.
int
azalia_reset(azalia_t *az)
{
	uint32_t gctl;
	int i;

	/* 4.2.2 Starting the High Definition Audio Controller */
	DPRINTF(("%s: resetting\n", __func__));
	gctl = AZ_READ_4(az, GCTL);
	AZ_WRITE_4(az, GCTL, gctl & ~HDA_GCTL_CRST);
	for (i = 5000; i > 0; i--) {
		az->io.delay(az->io.context, 10);
		if ((AZ_READ_4(az, GCTL) & HDA_GCTL_CRST) == 0)
			break;
	}
	DPRINTF(("%s: reset counter = %d\n", __func__, i));
	if (i == 0) {
		DPRINTF(("%s: reset failure\n", XNAME(az)));
		return(ETIMEDOUT);
	}
	az->io.delay(az->io.context, 1000);
	gctl = AZ_READ_4(az, GCTL);
	AZ_WRITE_4(az, GCTL, gctl | HDA_GCTL_CRST);
	for (i = 5000; i > 0; i--) {
		az->io.delay(az->io.context, 10);
		if (AZ_READ_4(az, GCTL) & HDA_GCTL_CRST)
			break;
	}
	DPRINTF(("%s: reset counter = %d\n", __func__, i));
	if (i == 0) {
		DPRINTF(("%s: reset-exit failure\n", XNAME(az)));
		return(ETIMEDOUT);
	}
	az->io.delay(az->io.context, 1000);

	return(0);
}

// From sys/dev/pci/azalia.c: azalia_set_command.
int
azalia_set_command(azalia_t *az, int caddr, nid_t nid, uint32_t control,
    uint32_t param)
{
	volatile corb_entry_t *corb;
	unsigned wp;
	uint32_t verb;
	uint16_t corbwp;

	if ((AZ_READ_1(az, CORBCTL) & HDA_CORBCTL_CORBRUN) == 0) {
		printf("%s: CORB is not running.\n", XNAME(az));
		return(-1);
	}
	verb = (caddr << 28) | (nid << 20) | (control << 8) | param;
    // Commands are serialized under the kernel lock, including across codecs.
    az->response_address = caddr;
	corbwp = AZ_READ_2(az, CORBWP);
	wp = corbwp & HDA_CORBWP_CORBWP;
	corb = (corb_entry_t*)az->corb_dma.addr;
	if (++wp >= az->corb_entries)
		wp = 0;
	corb[wp] = verb;

	AZ_WRITE_2(az, CORBWP, (corbwp & ~HDA_CORBWP_CORBWP) | wp);

	return(0);
}

// From sys/dev/pci/azalia.c: azalia_get_response.
int
azalia_get_response(azalia_t *az, uint32_t *result)
{
	const volatile rirb_entry_t *rirb;
	int i;
	uint16_t wp;

	if ((AZ_READ_1(az, RIRBCTL) & HDA_RIRBCTL_RIRBDMAEN) == 0) {
		printf("%s: RIRB is not running.\n", XNAME(az));
		return(-1);
	}

	rirb = (rirb_entry_t*)az->rirb_dma.addr;
	i = 5000;
	for (;;) {
		while (i > 0) {
			wp = AZ_READ_2(az, RIRBWP) & HDA_RIRBWP_RIRBWP;
			if (az->rirb_rp != wp)
				break;
			az->io.delay(az->io.context, 10);
			i--;
		}
		if (i == 0) {
			DPRINTF(("%s: RIRB time out\n", XNAME(az)));
			return(ETIMEDOUT);
		}
		if (++az->rirb_rp >= az->rirb_entries)
			az->rirb_rp = 0;
        // OpenBSD clears this in its interrupt handler. The polled adapter
        // must acknowledge each response to unblock QEMU's response counter.
        AZ_WRITE_1(az, RIRBSTS, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_RIRBOIS);
        if (!(rirb[az->rirb_rp].resp_ex & RIRB_RESP_UNSOL)) {
            if (RIRB_RESP_CODEC(rirb[az->rirb_rp].resp_ex) != az->response_address)
                return EIO;
            break;
        }
    }
	if (result != NULL)
		*result = rirb[az->rirb_rp].resp;

	return(0);
}

// From sys/dev/pci/azalia.c: azalia_widget_init_audio.
int
azalia_widget_init_audio(widget_t *self, const codec_t *codec)
{
	uint32_t result;
	int err;

	/* check audio format */
	if (self->widgetcap & COP_AWCAP_FORMATOV) {
		err = azalia_comresp(codec, self->nid, CORB_GET_PARAMETER,
		    COP_STREAM_FORMATS, &result);
		if (err)
			return err;
		self->d.audio.encodings = result;
		if (result == 0) { /* quirk for CMI9880.
				    * This must not occur usually... */
			self->d.audio.encodings =
			    codec->w[codec->audiofunc].d.audio.encodings;
			self->d.audio.bits_rates =
			    codec->w[codec->audiofunc].d.audio.bits_rates;
		} else {
			if ((result & COP_STREAM_FORMAT_PCM) == 0) {
				printf("%s: %s: No PCM support: %x\n",
				    XNAME(codec->az), self->name, result);
				return -1;
			}
			err = azalia_comresp(codec, self->nid,
			    CORB_GET_PARAMETER, COP_PCM, &result);
			if (err)
				return err;
			self->d.audio.bits_rates = result;
		}
	} else {
		self->d.audio.encodings =
		    codec->w[codec->audiofunc].d.audio.encodings;
		self->d.audio.bits_rates =
		    codec->w[codec->audiofunc].d.audio.bits_rates;
	}
	return 0;
}

// From sys/dev/pci/azalia.c: azalia_widget_init_pin.
int
azalia_widget_init_pin(widget_t *self, const codec_t *codec)
{
	uint32_t result, dir;
	int err;

	err = azalia_comresp(codec, self->nid, CORB_GET_CONFIGURATION_DEFAULT,
	    0, &result);
	if (err)
		return err;
	self->d.pin.config = result;
	self->d.pin.sequence = CORB_CD_SEQUENCE(result);
	self->d.pin.association = CORB_CD_ASSOCIATION(result);
	self->d.pin.color = CORB_CD_COLOR(result);
	self->d.pin.device = CORB_CD_DEVICE(result);

	err = azalia_comresp(codec, self->nid, CORB_GET_PARAMETER,
	    COP_PINCAP, &result);
	if (err)
		return err;
	self->d.pin.cap = result;

	dir = CORB_PWC_INPUT;
	switch (self->d.pin.device) {
	case CORB_CD_LINEOUT:
	case CORB_CD_SPEAKER:
	case CORB_CD_HEADPHONE:
	case CORB_CD_SPDIFOUT:
	case CORB_CD_DIGITALOUT:
		dir = CORB_PWC_OUTPUT;
		break;
	}

	if (dir == CORB_PWC_INPUT && !(self->d.pin.cap & COP_PINCAP_INPUT))
		dir = CORB_PWC_OUTPUT;
	if (dir == CORB_PWC_OUTPUT && !(self->d.pin.cap & COP_PINCAP_OUTPUT))
		dir = CORB_PWC_INPUT;

	if (dir == CORB_PWC_INPUT && self->d.pin.device == CORB_CD_MICIN) {
		if (COP_PINCAP_VREF(self->d.pin.cap) & (1 << CORB_PWC_VREF_80))
			dir |= CORB_PWC_VREF_80;
		else if (COP_PINCAP_VREF(self->d.pin.cap) &
		    (1 << CORB_PWC_VREF_50))
			dir |= CORB_PWC_VREF_50;
	}

	if ((codec->qrks & AZ_QRK_WID_OVREF50) && (dir == CORB_PWC_OUTPUT))
		dir |= CORB_PWC_VREF_50;

	err = azalia_comresp(codec, self->nid, CORB_SET_PIN_WIDGET_CONTROL,
	    dir, NULL);
	if (err)
		return err;

	if (self->d.pin.cap & COP_PINCAP_EAPD) {
		err = azalia_comresp(codec, self->nid,
		    CORB_GET_EAPD_BTL_ENABLE, 0, &result);
		if (err)
			return err;
		result &= 0xff;
		result |= CORB_EAPD_EAPD;
		err = azalia_comresp(codec, self->nid,
		    CORB_SET_EAPD_BTL_ENABLE, result, &result);
		if (err)
			return err;
	}

	/* Disable unconnected pins */
	if (CORB_CD_PORT(self->d.pin.config) == CORB_CD_NONE)
		self->enable = 0;

	return 0;
}

// From sys/dev/pci/azalia.c: azalia_stream_reset.
int
azalia_stream_reset(stream_t *self)
{
	int i;
	uint16_t ctl;
	uint8_t sts;

	/* Make sure RUN bit is zero before resetting */
	ctl = STR_READ_2(self, CTL);
	ctl &= ~HDA_SD_CTL_RUN;
	STR_WRITE_2(self, CTL, ctl);
	self->az->io.delay(self->az->io.context, 40);

	/* Start reset and wait for chip to enter. */
	ctl = STR_READ_2(self, CTL);
	STR_WRITE_2(self, CTL, ctl | HDA_SD_CTL_SRST);
	for (i = 5000; i > 0; i--) {
		self->az->io.delay(self->az->io.context, 10);
		ctl = STR_READ_2(self, CTL);
		if (ctl & HDA_SD_CTL_SRST)
			break;
	}
	if (i == 0) {
		DPRINTF(("%s: stream reset failure 1\n", XNAME(self->az)));
		return -1;
	}

	/* Clear reset and wait for chip to finish */
	STR_WRITE_2(self, CTL, ctl & ~HDA_SD_CTL_SRST);
	for (i = 5000; i > 0; i--) {
		self->az->io.delay(self->az->io.context, 10);
		ctl = STR_READ_2(self, CTL);
		if ((ctl & HDA_SD_CTL_SRST) == 0)
			break;
	}
	if (i == 0) {
		DPRINTF(("%s: stream reset failure 2\n", XNAME(self->az)));
		return -1;
	}

	sts = STR_READ_1(self, STS);
	sts |= HDA_SD_STS_DESE | HDA_SD_STS_FIFOE | HDA_SD_STS_BCIS;
	STR_WRITE_1(self, STS, sts);

	return (0);
}

// From sys/dev/pci/azalia.c: azalia_stream_start.
int
azalia_stream_start(stream_t *self)
{
	bdlist_entry_t *bdlist;
	bus_addr_t dmaaddr, dmaend;
	int err, index;
	uint8_t ctl2;

	err = azalia_stream_reset(self);
	if (err) {
		DPRINTF(("%s: stream reset failed\n", "azalia"));
		return err;
	}

	STR_WRITE_4(self, BDPL, 0);
	STR_WRITE_4(self, BDPU, 0);

	/* setup BDL */
	dmaaddr = AZALIA_DMA_DMAADDR(&self->buffer);
	dmaend = dmaaddr + self->bufsize;
	bdlist = (bdlist_entry_t*)self->bdlist.addr;
	for (index = 0; index < HDA_BDL_MAX; index++) {
		bdlist[index].low = (dmaaddr);
		bdlist[index].high = (PTR_UPPER32(dmaaddr));
		bdlist[index].length = (self->blk);
		bdlist[index].flags = 0 /* polled: no interrupt-on-completion */;
		dmaaddr += self->blk;
		if (dmaaddr >= dmaend) {
			index++;
			break;
		}
	}

	DPRINTFN(1, ("%s: size=%d fmt=0x%4.4x index=%d\n",
	    __func__, self->bufsize, self->fmt, index));

	dmaaddr = AZALIA_DMA_DMAADDR(&self->bdlist);
	STR_WRITE_4(self, BDPL, dmaaddr);
	STR_WRITE_4(self, BDPU, PTR_UPPER32(dmaaddr));
	STR_WRITE_2(self, LVI, (index - 1) & HDA_SD_LVI_LVI);
	ctl2 = STR_READ_1(self, CTL2);
	STR_WRITE_1(self, CTL2,
	    (ctl2 & ~HDA_SD_CTL2_STRM) | (self->number << HDA_SD_CTL2_STRM_SHIFT));
	STR_WRITE_4(self, CBL, self->bufsize);
	STR_WRITE_2(self, FMT, self->fmt);

	err = azalia_codec_connect_stream(self);
	if (err)
		return EINVAL;

	STR_WRITE_1(self, CTL, STR_READ_1(self, CTL) |
	    HDA_SD_CTL_RUN);
	return (0);
}

// From sys/dev/pci/azalia_codec.c: azalia_widget_enabled.
int
azalia_widget_enabled(const codec_t *self, nid_t nid)
{
	if (!VALID_WIDGET_NID(nid, self) || !self->w[nid].enable)
		return 0;
	return 1;
}

} // namespace azalia_port
