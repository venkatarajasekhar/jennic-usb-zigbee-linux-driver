/*
 * Driver for Jennic JN5139 IEEE802.15.4 micro-controller connected through
 * USB.
 *
 * Copyright (C) 2009
 * Telecooperation Office (TecO), Universitaet Karlsruhe (TH), Germany.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Author(s):
 * Philipp Scholl <scholl@teco.edu>
 *
 * This driver is based on the usbnet implementation and the fakehard driver of
 * the linux IEEE802.15.4 stack.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>

#include <net/mac802154.h>
#include <net/ieee802154.h>
#include <net/ieee802154_netdev.h>
#include <net/nl802154.h>
#include <net/wpan-phy.h>
#include "jenusb.h"

struct jenusb {
	struct usb_device    *udev;
	struct net_device    *net;
	struct usb_interface *interface;
	struct kref          kref;

	unsigned             in, in_cfm, out;

	unsigned long        read_delay;
	unsigned long        irq_delay;

	struct mutex         transaction;      /* mutex for buffers below */
	jenusb_req           req;              /* request buffer */
	jenusb_cfm           cfm;              /* confirm buffer */

	struct workqueue_struct *workqueue;
	bool                 running;

	atomic_t             panid, shortaddr;
};

struct cdc_ieee802154_desc {
	__u8   bLength;
	__u8   bDescriptorType;
	__u8   bDescriptorSubType;
	__u8   iMACAddress;
	__le16 wMaxSegmentSize;
} __attribute__ ((packed));

struct cdc_ieee802154 {
	struct usb_cdc_header_desc header;
	struct cdc_ieee802154_desc ieee802154;
};

struct jenusb_work {
	struct jenusb      *dev;
	struct jenusb_ind   ind;
	struct delayed_work dwork;
};

//#define jenusb_chk_err(cfm, attr) (printk("%s %s 0x%x\n",__func__,#cfm,cfm->mlme.attr.u8Status), __jenusb_chk_err(__func__, cfm, cfm->mlme.attr.u8Status))
#define jenusb_chk_err(cfm, attr) __jenusb_chk_err(__func__, cfm, cfm->mlme.attr.u8Status)
#define jenusb_post_req(dev,req,cfm) __jenusb_post_req(__func__, dev, req, cfm)

static int
__jenusb_chk_err(const char *s, jenusb_cfm *cfm, u8 reason)
{
	if (cfm->mlme.u8Status == MAC_MLME_CFM_ERROR) {
		printk("jenusb: %s failed 0x%x\n", s, reason);
		return true;
	}

	return false;
};

#define RETRIES 5

/* posts a request through usb to the device */
static int
__jenusb_post_req(const char *s, struct jenusb *dev, jenusb_req *req, jenusb_cfm *cfm)
{
	int retval, len, i=0;

	if (!dev->running)
		return -ENETDOWN;

	retval = usb_bulk_msg(dev->udev, dev->out, req, sizeof(*req), &len, HZ/2);

	if (retval) {
		err("req (write) from %s failed %d\n", s, retval);
		return retval;
	}

	do
	{
		retval = usb_interrupt_msg(dev->udev, dev->in_cfm, cfm, sizeof(*cfm), &len,
		                           HZ/2/RETRIES);
	} while (retval==-ETIMEDOUT && i++ < RETRIES);

	if (retval) {
		err("req (read) from %s failed. %d\n", s, retval);
		return retval;
	}

	if (cfm->type != req->type) {
		err("received different type of confirm as requested.\n");
		// TODO: this is bad -> stop the device
		return -EIO;
	}

	return retval;
}

static struct jenusb_work*
jenusb_work_alloc(struct jenusb *dev) {
	struct jenusb_work *work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (work) work->dev = dev;
	return work;
}

static void
ieee802154_addr_to_jenusb(struct ieee802154_addr* a, MAC_Addr_s *b)
{
	if (a == NULL) {
		b->u8AddrMode = IEEE802154_ADDR_SHORT;
		b->u16PanId = IEEE802154_PANID_BROADCAST;
		b->u16Short = cpu_to_be16(IEEE802154_ADDR_UNDEF);
		return;
	}

	BUG_ON(sizeof(a->hwaddr) != sizeof(b->sExt));

	b->u8AddrMode = a->addr_type;
	b->u16PanId = cpu_to_be16(a->pan_id);
	switch(a->addr_type) {

	case IEEE802154_ADDR_SHORT:
		b->u16Short = cpu_to_be16(a->short_addr);
		break;
	case IEEE802154_ADDR_LONG:
		memcpy(&b->sExt, a->hwaddr, sizeof(a->hwaddr));
		break;
	case IEEE802154_ADDR_NONE:
		b->u16Short = cpu_to_be16(IEEE802154_ADDR_UNDEF);
		break;
	default:
		printk("jenusb: undefined address\n");
	}
}

static void
jenusb_to_ieee802154_addr(MAC_Addr_s *a, struct ieee802154_addr* b)
{
	BUG_ON(sizeof(b->hwaddr) != sizeof(a->sExt));

	b->addr_type = a->u8AddrMode;
	b->pan_id = be16_to_cpu(a->u16PanId);
	switch(a->u8AddrMode) {
	case IEEE802154_ADDR_SHORT:
		b->short_addr = be16_to_cpu(a->u16Short);
		break;
	case IEEE802154_ADDR_LONG:
		memcpy(b->hwaddr, &a->sExt, sizeof(b->hwaddr));
		break;
	case IEEE802154_ADDR_NONE:
		b->short_addr = cpu_to_be16(IEEE802154_ADDR_UNDEF);
		break;
	default:
		printk("jenusb: undefined address\n");
	}
}

static u16 /* called in atomic context */
jenusb_get_pan_id(const struct net_device *net)
{
	struct jenusb *dev = netdev_priv(net);
	BUG_ON(net->type != ARPHRD_IEEE802154);
	return atomic_read(&dev->panid);
}

static u16 /* called in atomic context */
jenusb_get_short_addr(const struct net_device *net)
{
	struct jenusb *dev = netdev_priv(net);
	BUG_ON(net->type != ARPHRD_IEEE802154);
	return atomic_read(&dev->shortaddr);
}

static int
jenusb_set_panid(const struct net_device *net, u16 panid) {
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	BUG_ON(net->type != ARPHRD_IEEE802154);

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_SET;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqSet_s);
	req->mlme.sReqSet.u8PibAttribute = MAC_PIB_ATTR_PAN_ID;
	req->mlme.sReqSet.u8PibAttributeIndex = 0;
	req->mlme.sReqSet.u16PanId = panid;

	retval = jenusb_post_req(dev, req, cfm);
	if (!jenusb_chk_err(cfm, sCfmSet))
		atomic_set(&dev->panid, panid);

	mutex_unlock(&dev->transaction);
	return retval;
}

static int
jenusb_set_short_addr(struct net_device *net, u16 short_addr) {
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	BUG_ON(net->type != ARPHRD_IEEE802154);

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_SET;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqSet_s);
	req->mlme.sReqSet.u8PibAttribute = MAC_PIB_ATTR_SHORT_ADDRESS;
	req->mlme.sReqSet.u8PibAttributeIndex = 0;
	req->mlme.sReqSet.u16ShortAddr = short_addr;

	retval = jenusb_post_req(dev, req, cfm);
	if (!jenusb_chk_err(cfm, sCfmSet))
		atomic_set(&dev->shortaddr, short_addr);

	mutex_unlock(&dev->transaction);
	return retval;
}

static u8
jenusb_get_dsn(const struct net_device *dev)
{
	BUG_ON(dev->type != ARPHRD_IEEE802154);
	printk("jenusb: get_dsn\n");

	return 0x00; /* DSN are implemented in HW, so return devst 0 */
}

static u8
jenusb_get_bsn(const struct net_device *dev)
{
	BUG_ON(dev->type != ARPHRD_IEEE802154);

	printk("jenusb: get_bsn\n");
	return 0x00; /* BSN are implemented in HW, so return devst 0 */
}

static int
jenusb_assoc_req(struct net_device *net, struct ieee802154_addr *coord,
                 u8 channel, u8 page, u8 cap)
{
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval, shortaddr;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_ASSOCIATE;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqAssociate_s);
	ieee802154_addr_to_jenusb(coord, &req->mlme.sReqAssociate.sCoord);
	req->mlme.sReqAssociate.u8LogicalChan = channel;
	req->mlme.sReqAssociate.u8Capability = cap;
	req->mlme.sReqAssociate.u8SecurityEnable = false;

	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
		shortaddr = IEEE802154_ADDR_UNDEF;
	} else if (jenusb_chk_err(cfm, sCfmAssociate)) {
		shortaddr = IEEE802154_ADDR_UNDEF;
		retval = -EIO;
	} else {
		shortaddr = be16_to_cpu(cfm->mlme.sCfmAssociate.u16AssocShortAddr);
	}

	mutex_unlock(&dev->transaction);
	return retval;
}

static int
jenusb_assoc_resp(struct net_device *net, struct ieee802154_addr *addr,
                  u16 short_addr, u8 status)
{
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

  err("%s", __func__);
  return 0;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_RSP_ASSOCIATE;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeRspAssociate_s);

	if (addr->addr_type != IEEE802154_ADDR_LONG) {
		printk("jenusb: %s needs long addr\n", __func__);
		return -EINVAL;
	}
	req->mlme.sRspAssociate.sDeviceAddr.u32L = *(u32*) &addr->hwaddr[0];
	req->mlme.sRspAssociate.sDeviceAddr.u32H = *(u32*) &addr->hwaddr[4];
	req->mlme.sRspAssociate.u16AssocShortAddr = cpu_to_be16(short_addr);
	req->mlme.sRspAssociate.u8Status = status;
	req->mlme.sRspAssociate.u8SecurityEnable = false;

	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
	 // nothing to be done
	} else if (jenusb_chk_err(cfm, sCfmAssociate)) {
		retval = -EIO;
	}

	mutex_unlock(&dev->transaction);
	return retval;
}

static int
jenusb_disassoc_req(struct net_device *net, struct ieee802154_addr *addr, 
                    u8 reason)
{
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_DISASSOCIATE;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqDisassociate_s);
	ieee802154_addr_to_jenusb(addr, &req->mlme.sReqDisassociate.sAddr);
	req->mlme.sReqDisassociate.u8Reason = reason;
	req->mlme.sReqDisassociate.u8SecurityEnable = false;

	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
		// nothing to be done
	} else if (jenusb_chk_err(cfm, sCfmDisassociate)) {
		retval = -EIO;
	}

	mutex_unlock(&dev->transaction);
	return retval;
}

static int
jenusb_start_req(struct net_device *net, struct ieee802154_addr *addr,
                 u8 channel, u8 page, u8 bcn_ord, u8 sf_ord, u8 pan_coord, 
                 u8 blx, u8 coord_realign)
{
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval = 0;

	/* when we get started as a coordinator, the jennic chip switches to
	 * the IEEE 802.15.4 coord short addr (0x0000) */
	if (pan_coord) retval = jenusb_set_short_addr(net, 0x0000);
	if (retval) goto err;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	/* post the start request */
	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_START;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqStart_s);
	req->mlme.sReqStart.u16PanId = cpu_to_be16(addr->pan_id);
	req->mlme.sReqStart.u8Channel = channel;
	req->mlme.sReqStart.u8BeaconOrder = bcn_ord;
	req->mlme.sReqStart.u8SuperframeOrder = sf_ord;
	req->mlme.sReqStart.u8PanCoordinator = pan_coord;
	req->mlme.sReqStart.u8BatteryLifeExt = blx;
	req->mlme.sReqStart.u8Realignment = coord_realign;

	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
		// nothing to be done
	} else if (jenusb_chk_err(cfm, sCfmStart)) {
		retval = -EIO;
	}

err:
	mutex_unlock(&dev->transaction);
	return retval;
}

static int
jenusb_scan_req(struct net_device *net, u8 type, u32 channels,
                u8 page, u8 duration)
{
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;
	
	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_SCAN;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqScan_s);
	req->mlme.sReqScan.u32ScanChannels = cpu_to_be32(channels);
	req->mlme.sReqScan.u8ScanType = type; /* scan type defines are the same */
	req->mlme.sReqScan.u8ScanDuration = duration;

	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
		// nothing to be done
	} else if (jenusb_chk_err(cfm, sCfmScan)) {
		return -EIO;
	}

	mutex_unlock(&dev->transaction);
	return retval;
}

static struct ieee802154_mlme_ops jenusb_mlme_ops = {
	.assoc_req = 		jenusb_assoc_req,
	.assoc_resp = 		jenusb_assoc_resp,
	.disassoc_req = 	jenusb_disassoc_req,
	.start_req = 		jenusb_start_req,
	.scan_req = 		jenusb_scan_req,

	.get_pan_id = 		jenusb_get_pan_id,
	.get_short_addr = 	jenusb_get_short_addr,
	.get_dsn = 		jenusb_get_dsn,
	.get_bsn = 		jenusb_get_bsn,
};

struct fc { /* frame control field */
		u8 frame_type:3;
		u8 sec_enable:1;
		u8 fr_pending:1;
		u8 ack_required:1;
		u8 intra_pan:1;
		u8 reserved:3;
		u8 da_addr_mode:2;
		u8 reserved2:2;
		u8 sa_addr_mode:2;
} __attribute__((packed));

static int
addr_len(MAC_Addr_s *a) {
	switch(a->u8AddrMode) {
	case IEEE802154_ADDR_SHORT:
		return sizeof(u16);
	case IEEE802154_ADDR_LONG:
		return sizeof(u64);
	case IEEE802154_ADDR_NONE:
		return 0;
	default:
		err("unknown addr type");
	}
	return 0;
}

/* jennic chips only give the MSDU, so we rebuild a MPDU here,
 * but this is only true if not in promiscuous mode */
static int
to_skb(MAC_RxFrameData_s *frame, struct sk_buff *skb) {
	u8  *ptr;
	u8   seq;
	u8   isintrapan = false;
	int len = frame->u8SduLength;

	if (addr_len(&frame->sDstAddr) < 0 || addr_len(&frame->sSrcAddr) < 0)
		return -EINVAL;

 	len += sizeof(struct fc);
 	len += sizeof(seq);
	len += addr_len(&frame->sDstAddr);
	len += addr_len(&frame->sSrcAddr);

	skb = dev_alloc_skb(len);
	if (!skb) {
		if (printk_ratelimit()) err("rx no memory");
		return -ENOMEM;
	}

	isintrapan = frame->sDstAddr.u16PanId == frame->sSrcAddr.u16PanId;
	
	ptr = skb_put(skb, len);
	((struct fc*) ptr)->frame_type = IEEE802154_FC_TYPE_DATA;
	((struct fc*) ptr)->sec_enable = frame->u8SecurityUse;
	((struct fc*) ptr)->fr_pending = false;
	((struct fc*) ptr)->intra_pan = isintrapan;
	((struct fc*) ptr)->ack_required = false;
	((struct fc*) ptr)->da_addr_mode = frame->sDstAddr.u8AddrMode;
	((struct fc*) ptr)->sa_addr_mode = frame->sSrcAddr.u8AddrMode;

	*(ptr+2) = 0; /* sequence number, unable to get that */

	/* copy over addresses */
	ptr += 3;
	switch(frame->sDstAddr.u8AddrMode) {
	case IEEE802154_ADDR_NONE:
		break;
	case IEEE802154_ADDR_SHORT:
		*((u16*) ptr)     = be16_to_cpu(frame->sDstAddr.u16PanId);
		*((u16*) (ptr+2)) = be16_to_cpu(frame->sDstAddr.u16Short);
		ptr += 4;
		break;
	case IEEE802154_ADDR_LONG:
		*((u16*) ptr)     = be16_to_cpu(frame->sDstAddr.u16PanId);
		*((u64*) (ptr+2)) = be64_to_cpu(*((u64*) &frame->sDstAddr.sExt));
		ptr += 10;
		break;
	}

	switch(frame->sSrcAddr.u8AddrMode) {
	case IEEE802154_ADDR_NONE:
		break;
	case IEEE802154_ADDR_SHORT:
		if (isintrapan) {
			*((u16*) (ptr)) = be16_to_cpu(frame->sSrcAddr.u16Short);
			ptr += 2;
		} else {
			*((u16*) ptr)     = be16_to_cpu(frame->sSrcAddr.u16PanId);
			*((u16*) (ptr+2)) = be16_to_cpu(frame->sSrcAddr.u16Short);
			ptr += 4;
		}
		break;
	case IEEE802154_ADDR_LONG:
		*((u16*) ptr)     = be16_to_cpu(frame->sSrcAddr.u16PanId);
		*((u64*) (ptr+2)) = be64_to_cpu(*((u64*) &frame->sSrcAddr.sExt));
		ptr += 10;
		break;
	}

	memcpy(ptr, frame->au8Sdu, frame->u8SduLength);
	return len;
}

static void
jenusb_mcps_ind(struct net_device *dev, MAC_McpsDcfmInd_s *ind) {
	struct sk_buff *skb;
	MAC_RxFrameData_s *frame;
	int retval = 0;

	switch(ind->u8Type) {
		case MAC_MCPS_DCFM_PURGE: /* confirm for purge request */
			break;
		case MAC_MCPS_DCFM_DATA:  /* confirm for data send request */
			break;
		case MAC_MCPS_IND_DATA:   /* data received */
			retval = to_skb(&ind->sIndData.sFrame, skb);

			if (retval<0) {
				dev->stats.rx_dropped++;
				return;
			}

			skb->dev = dev;
			skb->skb_iif = skb->dev->ifindex;
			skb->protocol = htons(ETH_P_IEEE802154);
			skb_reset_mac_header(skb);
			//phy_cb(skb)->lqi = frame->u8LinkQuality;
			jenusb_to_ieee802154_addr(&frame->sSrcAddr, &mac_cb(skb)->sa);
			jenusb_to_ieee802154_addr(&frame->sDstAddr, &mac_cb(skb)->da);
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += retval;
			netif_rx(skb);
			break;
		default:
			err("jenusb: unknown mcps indication\n");
			break;
	}
}

static void
jenusb_mlme_ind(struct net_device *dev, MAC_MlmeDcfmInd_s *ind) {
	int retval = -ENOTSUPP;
	struct ieee802154_addr addr;

	switch(ind->u8Type) {
	case MAC_MLME_DCFM_SCAN:
			retval = ieee802154_nl_scan_confirm(dev,
				ind->sDcfmScan.u8Status,
				ind->sDcfmScan.u8ScanType,
				ind->sDcfmScan.u32UnscannedChannels,
				0,
				ind->sDcfmScan.u8ScanType == MAC_MLME_SCAN_TYPE_ENERGY_DETECT ?
				ind->sDcfmScan.au8EnergyDetect : NULL);
			break;
	case MAC_MLME_DCFM_ASSOCIATE:
			retval = ieee802154_nl_assoc_confirm(dev,
			  be16_to_cpu(ind->sDcfmAssociate.u16AssocShortAddr),
			  ind->sDcfmAssociate.u8Status);
			break;
	case MAC_MLME_DCFM_DISASSOCIATE:
			retval = ieee802154_nl_disassoc_confirm(dev,
			  ind->sDcfmDisassociate.u8Status);
			break;
	case MAC_MLME_IND_ASSOCIATE:
			addr.addr_type = IEEE802154_ADDR_LONG;
			memcpy(addr.hwaddr, &ind->sIndAssociate.sDeviceAddr, sizeof(addr.hwaddr));
			retval = ieee802154_nl_assoc_indic(dev, &addr,
			  ind->sIndAssociate.u8Capability); /* XXX: assume cap fields match */
			break;
	case MAC_MLME_IND_DISASSOCIATE:
			addr.addr_type = IEEE802154_ADDR_LONG;
			memcpy(addr.hwaddr, &ind->sIndDisassociate.sDeviceAddr, sizeof(addr.hwaddr));
			retval = ieee802154_nl_disassoc_indic(dev, &addr,
			  ind->sIndDisassociate.u8Reason); /* XXX: assume reason matches */
			break;

	case MAC_MLME_IND_BEACON_NOTIFY:
			retval = ieee802154_nl_beacon_indic(dev,
				be16_to_cpu(ind->sIndBeacon.sPANdescriptor.sCoord.u16PanId),
			  be16_to_cpu(ind->sIndBeacon.sPANdescriptor.sCoord.u16Short));
			break;

	case MAC_MLME_DCFM_GTS:
	case MAC_MLME_DCFM_POLL:
	case MAC_MLME_DCFM_RX_ENABLE:
	case MAC_MLME_IND_SYNC_LOSS:
	case MAC_MLME_IND_GTS:
	case MAC_MLME_IND_COMM_STATUS:
	case MAC_MLME_IND_ORPHAN:
			err("unsupported mlme indiccation 0x%x", ind->u8Type);
			break;

	default:
			err("unknown mlme indiccation\n");
			break;
	}

	if (retval) {
		err("%s delivery of 0x%x failed with %d", __func__, ind->u8Type, retval);
	}
}

static void
jenusb_rx_work(struct work_struct *w) {
	struct jenusb_work *work =
	  container_of(to_delayed_work(w), struct jenusb_work, dwork);
	struct jenusb      *dev  = work->dev;
	int retval = 0, len;

	if (!dev->running) {
		return;
	}

	retval = usb_bulk_msg(dev->udev, dev->in, &work->ind, sizeof(work->ind),
	                      &len, dev->read_delay*2);

	switch(retval) {
	case 0:
		break;
	case -ETIMEDOUT:
		if (len!=0)
			err("%s - incomplete read %d", __func__, len);
		goto again;
		break;
	case -ENODEV:
	case -EPIPE:
	case -ENOMEM:
	default:
		goto err;
		break;
	}

	switch(work->ind.type) {
	case MAC_SAP_MCPS:
		jenusb_mcps_ind(dev->net, &work->ind.mcps);
		break;
	case MAC_SAP_MLME:
		jenusb_mlme_ind(dev->net, &work->ind.mlme);
		break;
	default:
		if (printk_ratelimit())
			err("%s - unknown indication %d", __func__, work->ind.type);
	}

again:
	// reschedule this function
	INIT_DELAYED_WORK(&work->dwork, jenusb_rx_work);
	queue_delayed_work(dev->workqueue, &work->dwork, dev->read_delay);
	return;
err:
	// TODO: stop the device
	err("%s failed: %d", __func__, retval);
	if (work) kfree(work);
	return;
}

static int
jenusb_net_open(struct net_device *net) {
	struct jenusb *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	req->type = MAC_SAP_MLME;
	req->mlme.u8Type = MAC_MLME_REQ_RESET;
	req->mlme.u8ParamLength = sizeof(MAC_MlmeReqReset_s);
	req->mlme.sReqReset.u8SetDefaultPib = false;

	dev->running = true;
	retval = jenusb_post_req(dev, req, cfm);

	if (retval) {
		// do nothing
	} else if (jenusb_chk_err(cfm, sCfmReset)) {
		retval = -EIO;
	} else {
		struct jenusb_work *work = jenusb_work_alloc(dev);

		// schedule rx work
		if (!work) {
			dev->running = false;
			retval = -ENOMEM;
		} else {
			netif_start_queue(net);
			INIT_DELAYED_WORK(&work->dwork, jenusb_rx_work);
			queue_delayed_work(dev->workqueue, &work->dwork, dev->read_delay);
		}
	}

	if (retval)
		dev->running = false;
	mutex_unlock(&dev->transaction);

	return retval;
}

static int
jenusb_net_close(struct net_device *net) {
	struct jenusb *dev = netdev_priv(net);
	dev->running = false;
	netif_stop_queue(net);
	flush_workqueue(dev->workqueue);
	return 0;
}

static int
from_skb(struct sk_buff *skb, MAC_TxFrameData_s *frame) {
	u8 *ptr = skb->data;
	struct fc* fcf = (struct fc*) ptr;

	if (fcf->frame_type != IEEE802154_FC_TYPE_DATA)
		return -ENOTSUPP;

	if (fcf->da_addr_mode == IEEE802154_ADDR_NONE &&
	    fcf->sa_addr_mode == IEEE802154_ADDR_NONE)
		return -EINVAL;

	/* fill frame control, XXX: GTS missing */
	frame->u8TxOptions  = fcf->ack_required ? MAC_TX_OPTION_ACK : 0;
	frame->u8TxOptions |= fcf->sec_enable ? MAC_TX_OPTION_SECURITY : 0;

	/* sequence number, ignore can't set that manually */
	ptr += 2;

	/* copy addresses */
	ptr += 1;
	frame->sDstAddr.u8AddrMode = fcf->da_addr_mode;
	switch(fcf->da_addr_mode) {
	case IEEE802154_ADDR_NONE:
		break;
	case IEEE802154_ADDR_SHORT:
		frame->sDstAddr.u16PanId = cpu_to_be16(*((u16*) ptr));
		frame->sDstAddr.u16Short = cpu_to_be16(*((u16*) (ptr+2)));
		ptr += 4;
		break;
	case IEEE802154_ADDR_LONG:
		frame->sDstAddr.u16PanId = cpu_to_be16(*((u16*) ptr));
		memcpy(&frame->sDstAddr.sExt, ptr+2, sizeof(frame->sDstAddr.sExt));
		ptr += 10;
		break;
	default:
		return -EINVAL;
	}

	frame->sSrcAddr.u8AddrMode = fcf->sa_addr_mode;
	switch(fcf->da_addr_mode) {
	case IEEE802154_ADDR_NONE:
		break;
	case IEEE802154_ADDR_SHORT:
		frame->sSrcAddr.u16PanId = cpu_to_be16(*((u16*) ptr));
		frame->sSrcAddr.u16Short = cpu_to_be16(*((u16*) (ptr+2)));
		ptr += 4;
		break;
	case IEEE802154_ADDR_LONG:
		frame->sSrcAddr.u16PanId = cpu_to_be16(*((u16*) ptr));
		memcpy(&frame->sSrcAddr.sExt, ptr+2, sizeof(frame->sSrcAddr.sExt));
		ptr += 10;
		break;
	default:
		return -EINVAL;
	}

	memcpy(frame->au8Sdu, ptr, frame->u8SduLength);
	return frame->u8SduLength;
}

static int
jenusb_net_xmit(struct sk_buff *skb, struct net_device *net) {
	struct jenusb     *dev = netdev_priv(net);
	struct jenusb_req *req = &dev->req;
	struct jenusb_cfm *cfm = &dev->cfm;
	int retval;

	skb->skb_iif = net->ifindex;
	skb->dev = net;

	retval = mutex_lock_interruptible(&dev->transaction);
	if (retval) return retval;

	// fill mac request
	req->type = MAC_SAP_MCPS;
	req->mcps.u8Type = MAC_MCPS_REQ_DATA;
	req->mcps.u8ParamLength = sizeof(MAC_McpsReqData_s);
	req->mcps.sReqData.u8Handle = 0;

	if( (retval=from_skb(skb, &req->mcps.sReqData.sFrame)) < 0)
		goto drop;

	net->trans_start = jiffies;
	retval = jenusb_post_req(dev, req, cfm);

	if (retval)
		goto drop;

	if (cfm->mcps.u8Status == MAC_MCPS_CFM_ERROR) {
		switch (cfm->mcps.sCfmData.u8Status) {
			case MAC_ENUM_TRANSACTION_OVERFLOW:
				goto drop;
				break;
			default:
				if (printk_ratelimit())
					printk("jenusb: tx error %d\n", cfm->mcps.sCfmData.u8Status);
				netif_stop_queue(net);
				// XXX: probably bad stop the device
				break;
		}

		goto drop;
	}

	mutex_unlock(&dev->transaction);

	net->stats.tx_packets++;
	net->stats.tx_bytes += skb->len;
	dev_kfree_skb_any(skb);

	return NET_XMIT_SUCCESS;
drop:
	mutex_unlock(&dev->transaction);

	net->stats.tx_dropped++;
	dev_kfree_skb_any(skb);

	return NET_XMIT_DROP;
}
static int
jenusb_net_ioctl(struct net_device *dev, struct ifreq *ifr,
                 int cmd)
{
	struct sockaddr_ieee802154 *sa =
		(struct sockaddr_ieee802154 *)&ifr->ifr_addr;
	u16 pan_id, short_addr;
	int retval = 0;

	switch (cmd) {
	case SIOCGIFADDR:
		/* FIXME: fixed here, get from device IRL */
		pan_id = jenusb_get_pan_id(dev);
		short_addr = jenusb_get_short_addr(dev);
		if (pan_id == IEEE802154_PANID_BROADCAST ||
		    short_addr == IEEE802154_ADDR_BROADCAST)
			return -EADDRNOTAVAIL;

		sa->family = AF_IEEE802154;
		sa->addr.addr_type = IEEE802154_ADDR_SHORT;
		sa->addr.pan_id = pan_id;
		sa->addr.short_addr = short_addr;
		return 0;
	case SIOCSIFADDR:
		if (sa->family != AF_IEEE802154 ||
				sa->addr.addr_type != IEEE802154_ADDR_SHORT ||
				sa->addr.pan_id == IEEE802154_PANID_BROADCAST ||
				sa->addr.short_addr == IEEE802154_ADDR_BROADCAST ||
				sa->addr.short_addr == IEEE802154_ADDR_UNDEF)
			return -EINVAL;

		retval  = jenusb_set_panid(dev, sa->addr.pan_id);
		retval |= jenusb_set_short_addr(dev, sa->addr.short_addr);
		return retval;
	}
	return -ENOIOCTLCMD;
}

static int
jenusb_net_mac_addr(struct net_device *dev, void *p)
{
	return -EBUSY; /* HW address is built into the device */
}

static struct net_device_ops jenusb_net_ops = {
	.ndo_open = 		jenusb_net_open,
	.ndo_stop = 		jenusb_net_close,
	.ndo_start_xmit = 	jenusb_net_xmit,
	.ndo_do_ioctl = 	jenusb_net_ioctl,
	.ndo_set_mac_address = 	jenusb_net_mac_addr,
};

static void
jenusb_release(struct kref *kref)
{
	struct jenusb *dev = container_of(kref, struct jenusb, kref);

	dev->running = false;
	unregister_netdev(dev->net);

	if (dev->workqueue) {
		flush_workqueue(dev->workqueue);
		destroy_workqueue(dev->workqueue);
	}

	usb_put_dev(dev->udev);
	free_netdev(dev->net);
}

static void
ieee802154_setup(struct net_device *net)
{
	net->addr_len		= IEEE802154_ADDR_LEN;
	memset(net->broadcast, 0xff, IEEE802154_ADDR_LEN);
	net->features		= NETIF_F_NO_CSUM;
	net->needed_tailroom	= 2; /* FCS */
	net->mtu		= 127;
	net->tx_queue_len	= 10;
	net->type		= ARPHRD_IEEE802154;
	net->flags		= IFF_NOARP | IFF_BROADCAST;
	net->watchdog_timeo	= 0;
}

static u8 nibble(unsigned char c)
{
	if (likely(isdigit(c)))
		return c - '0';
	c = toupper(c);
	if (likely(isxdigit(c)))
		return 10 + c - 'A';
	return 0;
}

#define MAX_ALT_SETTINGS 32

int
jenusb_probe(struct usb_interface *interface, const struct usb_device_id *prod)
{
	struct jenusb *dev = NULL;
	struct net_device *net;
	struct usb_device *udev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct cdc_ieee802154 *info = NULL;
	size_t bulk_in_size, irq_in_size, irq_delay, read_delay;
	int retval = -ENOMEM, i, j;
	u32 bulkinep=0, bulkoutep=0, irqinep=0;

	udev = usb_get_dev(interface_to_usbdev(interface));

	/* set up endpoint information TODO: can do better */
	/* use first irq in ep and first bulk in/out eps */
	for(j = 0, iface_desc = interface->cur_altsetting;
	    j < MAX_ALT_SETTINGS && iface_desc;
	    ++j, iface_desc = usb_altnum_to_altsetting(interface, j)) {
		bulkinep = bulkoutep = irqinep = 0;

		for(i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
			endpoint = &iface_desc->endpoint[i].desc;

			if (!bulkoutep && usb_endpoint_is_bulk_out(endpoint)) {
				bulkoutep = endpoint->bEndpointAddress;
			}

			if (!bulkinep && usb_endpoint_is_bulk_in(endpoint)) {
				bulk_in_size = le16_to_cpu(endpoint->wMaxPacketSize);
				read_delay = endpoint->bInterval > 8 ?
				  endpoint->bInterval : 8;
				bulkinep = endpoint->bEndpointAddress;
			}

			if (!irqinep && usb_endpoint_is_int_in(endpoint)) {
				irq_in_size = le16_to_cpu(endpoint->wMaxPacketSize);
				irq_delay = endpoint->bInterval;
				irqinep = endpoint->bEndpointAddress;
			}
		}

		if (iface_desc->extralen == sizeof(struct cdc_ieee802154) ) {
			info = (struct cdc_ieee802154*) interface->cur_altsetting->extra;
		}

		if(bulkinep && bulkoutep && irqinep) {
			retval = usb_set_interface(udev,
			           iface_desc->desc.bInterfaceNumber,
			           iface_desc->desc.bAlternateSetting);
			if (retval) goto error;
			break;
		}
	}

	if(!(bulkinep && bulkoutep && irqinep)) {
		err("could not find needed endpoints");
		goto error;
	}

	if(!info) {
		err("cdc descriptor not found");
		goto error;
	}

	/* register the device now with the network stack */
	net = alloc_netdev(sizeof(*dev), "wpan%d", ieee802154_setup);

	if (!net) {
		retval = -ENOMEM;
		goto error;
	}

	/* retrieve mac address from usb descriptor, code adapted from usbnet. */
	{
		char buf[IEEE802154_ADDR_LEN*2+1];
		int len, i;

		BUG_ON(net->addr_len != IEEE802154_ADDR_LEN);

		len = usb_string(udev, info->ieee802154.iMACAddress, buf, sizeof(buf));

		if (len != IEEE802154_ADDR_LEN*2) {
			err("bad MAC string %d fetch, %d\n", info->ieee802154.iMACAddress, len);
			goto error;
		}

		for (i=len=0; i < IEEE802154_ADDR_LEN; i++, len+=2)
			net->dev_addr[i] = (nibble(buf[len])<<4) + nibble(buf[len+1]);

		memcpy(net->perm_addr, net->dev_addr, net->addr_len);
	}

	/* initialize driver */
	dev = netdev_priv(net);
	kref_init(&dev->kref);

	dev->running = false;
	dev->net = net;
	dev->udev = udev;
	dev->interface = interface;

	dev->irq_delay = msecs_to_jiffies(irq_delay);
	dev->read_delay = msecs_to_jiffies(read_delay);

	mutex_init(&dev->transaction);
	dev->workqueue = create_singlethread_workqueue(net->name);
	if (!dev->workqueue) {
		retval = -ENOMEM;
		goto error;
	}

	/* register our ops */
	net->netdev_ops = &jenusb_net_ops;
	net->ml_priv = &jenusb_mlme_ops;

	/* save our data pointers  */
	usb_set_intfdata(interface, dev);
	SET_NETDEV_DEV(net, &interface->dev);

	/* create pipes */
	dev->in = usb_rcvbulkpipe(interface_to_usbdev(interface), bulkinep);
	dev->in_cfm = usb_rcvintpipe(dev->udev, irqinep);
	dev->out = usb_sndbulkpipe(dev->udev, bulkoutep);

	retval = register_netdev(net);
	if (retval < 0) {
		err("unable to register network device");
		goto error;
	}

	return retval;
error:
	printk("jenusb: unable to initialize. Error numer %d\n", retval);
	if (dev) kref_put(&dev->kref, jenusb_release);
	return retval;
}
EXPORT_SYMBOL_GPL(jenusb_probe);

void
jenusb_disconnect(struct usb_interface *interface)
{
	struct jenusb *dev = usb_get_intfdata(interface);
	if (!dev) {
	  err("jensub: unable to get referenced driver");
	  return;
	}
	kref_put(&dev->kref, jenusb_release);
}
EXPORT_SYMBOL_GPL(jenusb_disconnect);

int
jenusb_suspend (struct usb_interface *intf, pm_message_t message)
{
	return 0;
}
EXPORT_SYMBOL_GPL(jenusb_suspend);

int
jenusb_resume (struct usb_interface *intf)
{
	return 0;
}
EXPORT_SYMBOL_GPL(jenusb_resume);


static const struct usb_device_id	products[] = {
	{ USB_DEVICE(0x0b6a, 0x0a93) },
	{ },
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver jenusb_driver = {
	.name = 	"jenusb",
	.id_table = 	products,
	.probe = 	jenusb_probe,
	.disconnect = 	jenusb_disconnect,
	.suspend = 	jenusb_suspend,
	.resume = 	jenusb_resume,
};

static __init int jenusb_init(void)
{
	int result;

	result = usb_register(&jenusb_driver);
	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static __exit void jenusb_exit(void)
{
	usb_deregister(&jenusb_driver);
}

module_init(jenusb_init);
module_exit(jenusb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Philipp Scholl <scholl@teco.edu>");
MODULE_DESCRIPTION("Jenusb Ieee802.15.4 Driver");
