/*
    ChibiOS - Copyright (C) 2006..2018 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    USBv2/hal_usb_lld.c
 * @brief   STM32 USB subsystem low level driver source.
 *
 * @addtogroup USB
 * @{
 */

#include <string.h>

#include "hal.h"

#if HAL_USE_USB || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

#define BTABLE_ADDR     0x0000

#define EPR_EP_TYPE_IS_ISO(bits)    ((bits & EPR_EP_TYPE_MASK) == EPR_EP_TYPE_ISO)

/**
 * @brief   Returns an endpoint descriptor pointer.
 */
#define USB_GET_DESCRIPTOR(ep)      (&USB_DRD_PMA_BUFF[ep])

/**
 * @brief   Gets the address of a RX buffer.
 */
#define USB_GET_RX_BUFFER(udp)      (uint32_t *)(USB_DRD_PMAADDR +       \
                                                ((udp)->RXBD & 0x0000FFFFU))

/**
 * @brief   Gets the address of a TX buffer.
 */
#define USB_GET_TX_BUFFER(ep)      (uint32_t *)(USB_DRD_PMAADDR +       \
                                               ((udp)->TXBD & 0x0000FFFFU))

/**
 * @brief   Gets the counter 0 of a RX buffer.
 */
#define USB_GET_RX_COUNT0(udp)     (size_t)(((udp)->RXBD >> 16) & 0x000003FFU)

/**
 * @brief   Gets the counter 0 of a RX buffer.
 */
#define USB_GET_TX_COUNT0(udp)     (size_t)(((udp)->TXBD >> 16) & 0x000003FFU)

/**
 * @brief   Gets the counter 1 of a RX buffer.
 */
#define USB_GET_RX_COUNT1(udp)     USB_GET_TX_COUNT0(udp)

/**
 * @brief   Gets the counter 1 of a TX buffer.
 */
#define USB_GET_TX_COUNT1(udp)     USB_GET_RX_COUNT0(udp)

/**
 * @brief   Sets the counter 0 of a RX buffer.
 */
#define USB_SET_RX_COUNT0(udp, n) do {                                      \
  (udp)->RXBD = (((udp)->RXBD & ~0x03FF0000U) | ((uint32_t)(n) << 16));     \
} while (false)

/**
 * @brief   Sets the counter 0 of a TX buffer.
 */
#define USB_SET_TX_COUNT0(udp, n) do {                                      \
  (udp)->TXBD = (((udp)->TXBD & ~0x03FF0000U) | ((uint32_t)(n) << 16));     \
} while (false)

/**
 * @brief   Sets the counter 1 of a RX buffer.
 */
#define USB_SET_RX_COUNT1(udp, n)   USB_SET_TX_COUNT0(udp, n)

/**
 * @brief   Sets the counter 1 of a TX buffer.
 */
#define USB_SET_TX_COUNT1(udp, n)   USB_SET_RX_COUNT0(udp, n)

/**
 * @brief   Mask of all the toggling bits in the EPR register.
 */
#define CHEPR_TOGGLE_MASK       (USB_CHEP_TX_STTX_Msk |                     \
                                 USB_CHEP_DTOG_TX_Msk |                     \
                                 USB_CHEP_RX_STRX_Msk |                     \
                                 USB_CHEP_DTOG_RX_Msk |                     \
                                 USB_CHEP_SETUP_Msk)

/**
 * @brief   Clears the VTRX bit.
 */
#define CHEPR_CLEAR_VTRX(usbp, ep)                                          \
  (usbp)->usb->CHEPR[ep] = ((usbp)->usb->CHEPR[ep] & ~USB_EP_VTRX & ~CHEPR_TOGGLE_MASK) | USB_EP_VTTX

/**
 * @brief   Clears the VTTX bit.
 */
#define CHEPR_CLEAR_VTTX(usbp, ep)                                          \
  (usbp)->usb->CHEPR[ep] = ((usbp)->usb->CHEPR[ep] & ~USB_EP_VTTX & ~CHEPR_TOGGLE_MASK) | USB_EP_VTRX

/**
 * @brief   Sets the STATRX field.
 */
#define CHEPR_SET_STATRX(usbp, ep, epr)                                     \
  (usbp)->usb->CHEPR[ep] = (((usbp)->usb->CHEPR[ep] &                       \
                            ~(CHEPR_TOGGLE_MASK & ~USB_CHEP_RX_STRX_Msk)) ^ \
                           (epr)) | EPR_CTR_MASK

/**
 * @brief   Sets the STATTX field.
 */
#define CHEPR_SET_STATTX(usbp, ep, epr)                                     \
  (usbp)->usb->CHEPR[ep] = (((usbp)->usb->CHEPR[ep] &                       \
                            ~(CHEPR_TOGGLE_MASK & ~USB_CHEP_TX_STTX_Msk)) ^ \
                           (epr)) | EPR_CTR_MASK

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/** @brief USB1 driver identifier.*/
#if STM32_USB_USE_USB1 || defined(__DOXYGEN__)
USBDriver USBD1;
#endif

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/

/**
 * @brief   EP0 state.
 * @note    It is an union because IN and OUT endpoints are never used at the
 *          same time for EP0.
 */
static union {
  /**
   * @brief   IN EP0 state.
   */
  USBInEndpointState in;
  /**
   * @brief   OUT EP0 state.
   */
  USBOutEndpointState out;
} ep0_state;

/**
 * @brief   Buffer for the EP0 setup packets.
 */
static uint8_t ep0setup_buffer[8];

/**
 * @brief   EP0 initialization structure.
 */
static const USBEndpointConfig ep0config = {
  USB_EP_MODE_TYPE_CTRL,
  _usb_ep0setup,
  _usb_ep0in,
  _usb_ep0out,
  0x40,
  0x40,
  &ep0_state.in,
  &ep0_state.out,
  1,
  ep0setup_buffer
};

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 */
static void usb_pm_reset(USBDriver *usbp) {

  /* The first 64 bytes are reserved for the descriptors table. The effective
     available RAM.*/
  usbp->pmnext = 64U;
}

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] size      size of the packet buffer to allocate
 * @return              The packet buffer address.
 */
static uint32_t usb_pm_alloc(USBDriver *usbp, size_t size) {
  uint32_t next;

  next = usbp->pmnext;
  usbp->pmnext += (size + 3) & ~3;

  osalDbgAssert(usbp->pmnext <= STM32_USB_PMA_SIZE, "PMA overflow");

  return next;
}

/**
 * @brief   Reads from a dedicated packet buffer.
 *
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 * @return              The size of the receivee packet.
 *
 * @notapi
 */
static size_t usb_packet_read_to_buffer(usbep_t ep, uint8_t *buf) {
  size_t i, n;
  uint32_t w;
  USB_DRD_PMABuffDescTypeDef *udp = USB_GET_DESCRIPTOR(ep);
  uint32_t *pmap = USB_GET_RX_BUFFER(udp);
#if STM32_USB_USE_ISOCHRONOUS
  uint32_t epr = STM32_USB->EPR[ep];

  /* Double buffering is always enabled for isochronous endpoints, and
     although we overlap the two buffers for simplicity, we still need
     to read from the right counter. The DTOG_RX bit indicates the buffer
     that is currently in use by the USB peripheral, that is, the buffer
     in which the next received packet will be stored, so we need to
     read the counter of the OTHER buffer, which is where the last
     received packet was stored.*/
  if (EPR_EP_TYPE_IS_ISO(epr) && !(epr & EPR_DTOG_RX)) {
    n = USB_GET_RX_COUNT1(udp);
  }
  else {
    n = USB_GET_RX_COUNT0(udp);
  }
#else
  n = USB_GET_RX_COUNT0(udp);
#endif

  i = n;

#if STM32_USB_USE_FAST_COPY /* TODO */
  while (i >= 16) {
    uint32_t w;

    w = *(pmap + 0);
    *(buf + 0) = (uint8_t)w;
    *(buf + 1) = (uint8_t)(w >> 8);
    w = *(pmap + 1);
    *(buf + 2) = (uint8_t)w;
    *(buf + 3) = (uint8_t)(w >> 8);
    w = *(pmap + 2);
    *(buf + 4) = (uint8_t)w;
    *(buf + 5) = (uint8_t)(w >> 8);
    w = *(pmap + 3);
    *(buf + 6) = (uint8_t)w;
    *(buf + 7) = (uint8_t)(w >> 8);
    w = *(pmap + 4);
    *(buf + 8) = (uint8_t)w;
    *(buf + 9) = (uint8_t)(w >> 8);
    w = *(pmap + 5);
    *(buf + 10) = (uint8_t)w;
    *(buf + 11) = (uint8_t)(w >> 8);
    w = *(pmap + 6);
    *(buf + 12) = (uint8_t)w;
    *(buf + 13) = (uint8_t)(w >> 8);
    w = *(pmap + 7);
    *(buf + 14) = (uint8_t)w;
    *(buf + 15) = (uint8_t)(w >> 8);

    i -= 16;
    buf += 16;
    pmap += 8;
  }
#endif /* STM32_USB_USE_FAST_COPY */

  while (i >= 4U) {
    w = *pmap++;
    if (i < 4U) {
      break;
    }
    *buf++ = (uint8_t)w;
    *buf++ = (uint8_t)(w >> 8);
    *buf++ = (uint8_t)(w >> 16);
    *buf++ = (uint8_t)(w >> 24);
    i -= 4U;
  }

  if (i >= 1) { /* TODO */
    *buf = (uint8_t)*pmap;
  }

  return n;
}

/**
 * @brief   Writes to a dedicated packet buffer.
 *
 * @param[in] ep        endpoint number
 * @param[in] buf       buffer where to fetch the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_write_from_buffer(usbep_t ep,
                                         const uint8_t *buf,
                                         size_t n) {
  USB_DRD_PMABuffDescTypeDef *udp = USB_GET_DESCRIPTOR(ep);
  uint32_t *pmap = USB_GET_TX_BUFFER(udp);
  int i = (int)n;

#if STM32_USB_USE_ISOCHRONOUS
  uint32_t epr = STM32_USB->EPR[ep];

  /* Double buffering is always enabled for isochronous endpoints, and
     although we overlap the two buffers for simplicity, we still need
     to write to the right counter. The DTOG_TX bit indicates the buffer
     that is currently in use by the USB peripheral, that is, the buffer
     from which the next packet will be sent, so we need to write the
     counter of that buffer.*/
  if (EPR_EP_TYPE_IS_ISO(epr) && (epr & EPR_DTOG_TX)) {
    USB_SET_TX_COUNT1(udp, n);
  }
  else {
    USB_SET_TX_COUNT0(udp, n);
  }
#else
  USB_SET_TX_COUNT0(udp, n);
#endif

#if STM32_USB_USE_FAST_COPY /* TODO */
  while (i >= 16) {
    uint32_t w;

    w  = *(buf + 0);
    w |= *(buf + 1) << 8;
    *(pmap + 0) = (stm32_usb_pma_t)w;
    w  = *(buf + 2);
    w |= *(buf + 3) << 8;
    *(pmap + 1) = (stm32_usb_pma_t)w;
    w  = *(buf + 4);
    w |= *(buf + 5) << 8;
    *(pmap + 2) = (stm32_usb_pma_t)w;
    w  = *(buf + 6);
    w |= *(buf + 7) << 8;
    *(pmap + 3) = (stm32_usb_pma_t)w;
    w  = *(buf + 8);
    w |= *(buf + 9) << 8;
    *(pmap + 4) = (stm32_usb_pma_t)w;
    w  = *(buf + 10);
    w |= *(buf + 11) << 8;
    *(pmap + 5) = (stm32_usb_pma_t)w;
    w  = *(buf + 12);
    w |= *(buf + 13) << 8;
    *(pmap + 6) = (stm32_usb_pma_t)w;
    w  = *(buf + 14);
    w |= *(buf + 15) << 8;
    *(pmap + 7) = (stm32_usb_pma_t)w;

    i -= 16;
    buf += 16;
    pmap += 8;
  }
#endif /* STM32_USB_USE_FAST_COPY */

  while (i > 0) {
    uint32_t w;

    w  = (uint32_t)(*buf++);
    w |= (uint32_t)(*buf++) << 8;
    w |= (uint32_t)(*buf++) << 16;
    w |= (uint32_t)(*buf++) << 24;
    *pmap++ = w;
    i -= 4U;
  }
}

/**
 * @brief   Common ISR code, serves the EP-related interrupts.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
static void usb_serve_endpoints(USBDriver *usbp, uint32_t ep) {
  size_t n;
  uint32_t chepr = usbp->usb->CHEPR[ep];
  const USBEndpointConfig *epcp = usbp->epc[ep];

  if (chepr & USB_EP_VTTX) {
    /* IN endpoint, transmission.*/
    USBInEndpointState *isp = epcp->in_state;

    CHEPR_CLEAR_VTTX(usbp, ep);

    isp->txcnt += isp->txlast;
    n = isp->txsize - isp->txcnt;
    if (n > 0) {
      /* Transfer not completed, there are more packets to send.*/
      if (n > epcp->in_maxsize)
        n = epcp->in_maxsize;

      /* Writes the packet from the defined buffer.*/
      isp->txbuf += isp->txlast;
      isp->txlast = n;
      usb_packet_write_from_buffer(ep, isp->txbuf, n);

      /* Starting IN operation.*/
      CHEPR_SET_STATTX(usbp, ep, EPR_STAT_TX_VALID);
    }
    else {
      /* Transfer completed, invokes the callback.*/
      _usb_isr_invoke_in_cb(usbp, ep);
    }
  }
  if (chepr & USB_EP_VTRX) {
    /* OUT endpoint, receive.*/

    CHEPR_CLEAR_VTRX(usbp, ep);

    if (chepr & USB_EP_SETUP) {
      /* Setup packets handling, setup packets are handled using a
         specific callback.*/
      _usb_isr_invoke_setup_cb(usbp, ep);
    }
    else {
      USBOutEndpointState *osp = epcp->out_state;

      /* Reads the packet into the defined buffer.*/
      n = usb_packet_read_to_buffer(ep, osp->rxbuf);
      osp->rxbuf += n;

      /* Transaction data updated.*/
      osp->rxcnt  += n;
      osp->rxsize -= n;
      osp->rxpkts -= 1;

      /* The transaction is completed if the specified number of packets
         has been received or the current packet is a short packet.*/
      if ((n < epcp->out_maxsize) || (osp->rxpkts == 0)) {
        /* Transfer complete, invokes the callback.*/
        _usb_isr_invoke_out_cb(usbp, ep);
      }
      else {
        /* Transfer not complete, there are more packets to receive.*/
        CHEPR_SET_STATRX(usbp, ep, EPR_STAT_RX_VALID);
      }
    }
  }
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

#if STM32_USB_USE_USB1 || defined(__DOXYGEN__)
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
#if STM32_USB_USE_ISOCHRONOUS
/**
 * @brief   USB high priority interrupt handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(STM32_USB1_HP_HANDLER) {
  uint32_t istr;
  USBDriver *usbp = &USBD1;

  OSAL_IRQ_PROLOGUE();

  /* Endpoint events handling.*/
  istr = STM32_USB->ISTR;
  while (istr & ISTR_CTR) {
    usb_serve_endpoints(usbp, istr & ISTR_EP_ID_MASK);
    istr = STM32_USB->ISTR;
  }

  OSAL_IRQ_EPILOGUE();
}
#endif /* STM32_USB_USE_ISOCHRONOUS */
#endif /* STM32_USB1_LP_NUMBER != STM32_USB1_HP_NUMBER */

/**
 * @brief   USB low priority interrupt handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(STM32_USB1_LP_HANDLER) {
  uint32_t istr;
  USBDriver *usbp = &USBD1;

  OSAL_IRQ_PROLOGUE();

  istr = STM32_USB->ISTR;

  /* USB bus reset condition handling.*/
  if (istr & ISTR_RESET) {
    STM32_USB->ISTR = ~ISTR_RESET;

    _usb_reset(usbp);
  }

  /* USB bus SUSPEND condition handling.*/
  if (istr & ISTR_SUSP) {
    STM32_USB->CNTR |= CNTR_FSUSP;
#if STM32_USB_LOW_POWER_ON_SUSPEND
    STM32_USB->CNTR |= CNTR_LP_MODE;
#endif
    STM32_USB->ISTR = ~ISTR_SUSP;

    _usb_suspend(usbp);
  }

  /* USB bus WAKEUP condition handling.*/
  if (istr & ISTR_WKUP) {
    uint32_t fnr = STM32_USB->FNR;
    if (!(fnr & FNR_RXDP)) {
      STM32_USB->CNTR &= ~CNTR_FSUSP;

      _usb_wakeup(usbp);
    }
#if STM32_USB_LOW_POWER_ON_SUSPEND
    else {
      /* Just noise, going back in SUSPEND mode, reference manual 22.4.5,
         table 169.*/
      STM32_USB->CNTR |= CNTR_LP_MODE;
    }
#endif
    STM32_USB->ISTR = ~ISTR_WKUP;
  }

  /* SOF handling.*/
  if (istr & ISTR_SOF) {
    _usb_isr_invoke_sof_cb(usbp);
    STM32_USB->ISTR = ~ISTR_SOF;
  }

  /* Endpoint events handling.*/
  while (istr & ISTR_CTR) {
    usb_serve_endpoints(usbp, istr & ISTR_EP_ID_MASK);
    istr = STM32_USB->ISTR;
  }

  OSAL_IRQ_EPILOGUE();
}
#endif /* STM32_USB_USE_USB1 */

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level USB driver initialization.
 *
 * @notapi
 */
void usb_lld_init(void) {

  /* Driver initialization.*/
  usbObjectInit(&USBD1);
}

/**
 * @brief   Configures and activates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_start(USBDriver *usbp) {

  if (usbp->state == USB_STOP) {
    /* Clock activation.*/
#if STM32_USB_USE_USB1
    if (&USBD1 == usbp) {

      osalDbgAssert((STM32_USBCLK >= (48000000U - STM32_USB_48MHZ_DELTA)) &&
                    (STM32_USBCLK <= (48000000U + STM32_USB_48MHZ_DELTA)),
                    "invalid clock frequency");

      /* USB clock enabled.*/
      rccEnableUSB(true);
      /* Powers up the transceiver while holding the USB in reset state.*/
      STM32_USB->CNTR = CNTR_FRES;
      /* Enabling the USB IRQ vectors, this also gives enough time to allow
         the transceiver power up (1uS).*/
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
      nvicEnableVector(STM32_USB1_HP_NUMBER, STM32_USB_USB1_HP_IRQ_PRIORITY);
#endif
      nvicEnableVector(STM32_USB1_LP_NUMBER, STM32_USB_USB1_LP_IRQ_PRIORITY);
      /* Releases the USB reset.*/
      STM32_USB->CNTR = 0;
    }
#endif
    /* Reset procedure enforced on driver start.*/
    usb_lld_reset(usbp);
  }
}

/**
 * @brief   Deactivates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_stop(USBDriver *usbp) {

  /* If in ready state then disables the USB clock.*/
  if (usbp->state != USB_STOP) {
#if STM32_USB_USE_USB1
    if (&USBD1 == usbp) {
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
      nvicDisableVector(STM32_USB1_HP_NUMBER);
#endif
      nvicDisableVector(STM32_USB1_LP_NUMBER);
      STM32_USB->CNTR = CNTR_PDWN | CNTR_FRES;
      rccDisableUSB();
    }
#endif
  }
}

/**
 * @brief   USB low level reset routine.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_reset(USBDriver *usbp) {
  uint32_t cntr;

  /* Post reset initialization.*/
  STM32_USB->BTABLE = BTABLE_ADDR;
  STM32_USB->ISTR   = 0;
  STM32_USB->DADDR  = DADDR_EF;
  cntr              = /* CNTR_ESOFM | */ CNTR_RESETM  | CNTR_SUSPM |
                      CNTR_WKUPM | /* CNTR_ERRM | CNTR_PMAOVRM |*/ CNTR_CTRM;
  /* The SOF interrupt is only enabled if a callback is defined for
     this service because it is an high rate source.*/
  if (usbp->config->sof_cb != NULL)
    cntr |= CNTR_SOFM;
  STM32_USB->CNTR = cntr;

  /* Resets the packet memory allocator.*/
  usb_pm_reset(usbp);

  /* EP0 initialization.*/
  usbp->epc[0] = &ep0config;
  usb_lld_init_endpoint(usbp, 0);
}

/**
 * @brief   Sets the USB address.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_set_address(USBDriver *usbp) {

  STM32_USB->DADDR = (uint32_t)(usbp->address) | DADDR_EF;
}

/**
 * @brief   Enables an endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_init_endpoint(USBDriver *usbp, usbep_t ep) {
  uint16_t epr;
  USB_DRD_PMABuffDescTypeDef *dp;
  const USBEndpointConfig *epcp = usbp->epc[ep];

  /* Setting the endpoint type. Note that isochronous endpoints cannot be
     bidirectional because it uses double buffering and both transmit and
     receive descriptor fields are used for either direction.*/
  switch (epcp->ep_mode & USB_EP_MODE_TYPE) {
  case USB_EP_MODE_TYPE_ISOC:
#if STM32_USB_USE_ISOCHRONOUS
    osalDbgAssert((epcp->in_state == NULL) || (epcp->out_state == NULL),
                  "isochronous EP cannot be IN and OUT");
    epr = EPR_EP_TYPE_ISO;
    break;
#else
    osalDbgAssert(false, "isochronous support disabled");
#endif
    /* Falls through.*/
  case USB_EP_MODE_TYPE_BULK:
    epr = EPR_EP_TYPE_BULK;
    break;
  case USB_EP_MODE_TYPE_INTR:
    epr = EPR_EP_TYPE_INTERRUPT;
    break;
  default:
    epr = EPR_EP_TYPE_CONTROL;
  }

  dp = USB_GET_DESCRIPTOR(ep);

  /* IN endpoint handling.*/
  if (epcp->in_state != NULL) {
    dp->TXCOUNT0 = 0;
    dp->TXADDR0  = usb_pm_alloc(usbp, epcp->in_maxsize);

#if STM32_USB_USE_ISOCHRONOUS
    if (epr == EPR_EP_TYPE_ISO) {
      epr |= EPR_STAT_TX_VALID;
      dp->TXCOUNT1 = dp->TXCOUNT0;
      dp->TXADDR1  = dp->TXADDR0;   /* Both buffers overlapped.*/
    }
    else {
      epr |= EPR_STAT_TX_NAK;
    }
#else
    epr |= EPR_STAT_TX_NAK;
#endif
  }

  /* OUT endpoint handling.*/
  if (epcp->out_state != NULL) {
    uint16_t nblocks;

    /* Endpoint size and address initialization.*/
    if (epcp->out_maxsize > 62)
      nblocks = (((((epcp->out_maxsize - 1) | 0x1f) + 1) / 32) << 10) |
                0x8000;
    else
      nblocks = ((((epcp->out_maxsize - 1) | 1) + 1) / 2) << 10;
    dp->RXCOUNT0 = nblocks;
    dp->RXADDR0  = usb_pm_alloc(usbp, epcp->out_maxsize);

#if STM32_USB_USE_ISOCHRONOUS
    if (epr == EPR_EP_TYPE_ISO) {
      epr |= EPR_STAT_RX_VALID;
      dp->RXCOUNT1 = dp->RXCOUNT0;
      dp->RXADDR1  = dp->RXADDR0;   /* Both buffers overlapped.*/
    }
    else {
      epr |= EPR_STAT_RX_NAK;
    }
#else
    epr |= EPR_STAT_RX_NAK;
#endif
  }

  /* Resetting the data toggling bits for this endpoint.*/
  if (STM32_USB->EPR[ep] & EPR_DTOG_RX) {
    epr |= EPR_DTOG_RX;
  }

  if (STM32_USB->EPR[ep] & EPR_DTOG_TX) {
    epr |= EPR_DTOG_TX;
  }

  /* EPxR register setup.*/
  EPR_SET(ep, epr | ep);
  EPR_TOGGLE(ep, epr);
}

/**
 * @brief   Disables all the active endpoints except the endpoint zero.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_disable_endpoints(USBDriver *usbp) {
  unsigned i;

  /* Resets the packet memory allocator.*/
  usb_pm_reset(usbp);

  /* Disabling all endpoints.*/
  for (i = 1; i <= USB_ENDOPOINTS_NUMBER; i++) {
    EPR_TOGGLE(i, 0);
    EPR_SET(i, 0);
  }
}

/**
 * @brief   Returns the status of an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;
  switch (STM32_USB->EPR[ep] & EPR_STAT_RX_MASK) {
  case EPR_STAT_RX_DIS:
    return EP_STATUS_DISABLED;
  case EPR_STAT_RX_STALL:
    return EP_STATUS_STALLED;
  default:
    return EP_STATUS_ACTIVE;
  }
}

/**
 * @brief   Returns the status of an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;
  switch (STM32_USB->EPR[ep] & EPR_STAT_TX_MASK) {
  case EPR_STAT_TX_DIS:
    return EP_STATUS_DISABLED;
  case EPR_STAT_TX_STALL:
    return EP_STATUS_STALLED;
  default:
    return EP_STATUS_ACTIVE;
  }
}

/**
 * @brief   Reads a setup packet from the dedicated packet buffer.
 * @details This function must be invoked in the context of the @p setup_cb
 *          callback in order to read the received setup packet.
 * @pre     In order to use this function the endpoint must have been
 *          initialized as a control endpoint.
 * @post    The endpoint is ready to accept another packet.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 *
 * @notapi
 */
void usb_lld_read_setup(USBDriver *usbp, usbep_t ep, uint8_t *buf) {
  stm32_usb_pma_t *pmap;
  USB_DRD_PMABuffDescTypeDef *udp;
  uint32_t n;

  (void)usbp;
  udp = USB_GET_DESCRIPTOR(ep);
  pmap = USB_ADDR2PTR(udp->RXADDR0);
  for (n = 0; n < 4; n++) {
    *(uint16_t *)(void *)buf = (uint16_t)*pmap++;
    buf += 2;
  }
}

/**
 * @brief   Starts a receive operation on an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_out(USBDriver *usbp, usbep_t ep) {
  USBOutEndpointState *osp = usbp->epc[ep]->out_state;

  /* Transfer initialization.*/
  if (osp->rxsize == 0)         /* Special case for zero sized packets.*/
    osp->rxpkts = 1;
  else
    osp->rxpkts = (uint16_t)((osp->rxsize + usbp->epc[ep]->out_maxsize - 1) /
                             usbp->epc[ep]->out_maxsize);

  EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);
}

/**
 * @brief   Starts a transmit operation on an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_in(USBDriver *usbp, usbep_t ep) {
  size_t n;
  USBInEndpointState *isp = usbp->epc[ep]->in_state;

  /* Transfer initialization.*/
  n = isp->txsize;
  if (n > (size_t)usbp->epc[ep]->in_maxsize)
    n = (size_t)usbp->epc[ep]->in_maxsize;

  isp->txlast = n;
  usb_packet_write_from_buffer(ep, isp->txbuf, n);

  EPR_SET_STAT_TX(ep, EPR_STAT_TX_VALID);
}

/**
 * @brief   Brings an OUT endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_RX(ep, EPR_STAT_RX_STALL);
}

/**
 * @brief   Brings an IN endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_TX(ep, EPR_STAT_TX_STALL);
}

/**
 * @brief   Brings an OUT endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  /* Makes sure to not put to NAK an endpoint that is already
     transferring.*/
  if ((STM32_USB->EPR[ep] & EPR_STAT_RX_MASK) != EPR_STAT_RX_VALID)
    EPR_SET_STAT_TX(ep, EPR_STAT_RX_NAK);
}

/**
 * @brief   Brings an IN endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  /* Makes sure to not put to NAK an endpoint that is already
     transferring.*/
  if ((STM32_USB->EPR[ep] & EPR_STAT_TX_MASK) != EPR_STAT_TX_VALID)
    EPR_SET_STAT_TX(ep, EPR_STAT_TX_NAK);
}

#endif /* HAL_USE_USB */

/** @} */
