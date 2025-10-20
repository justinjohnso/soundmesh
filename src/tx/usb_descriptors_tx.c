#include <tusb.h>

// Audio constants
#define AUDIO_CLOCK_SOURCE_ID 0x04
#define AUDIO_IN_TERM_ID 0x01
#define AUDIO_OUT_TERM_ID 0x03

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
  static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,  // USB 2.0
    .bDeviceClass = TUSB_CLASS_AUDIO,
    .bDeviceSubClass = AUDIO_SUBCLASS_AUDIOCONTROL,
    .bDeviceProtocol = AUDIO_INT_PROTOCOL_CODE_UNDEF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,  // Placeholder
    .idProduct = 0x4004, // Placeholder
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01
  };
  return (uint8_t const *)&desc_device;
}

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;  // for multiple configurations

  static uint8_t const desc_audio[] = {
    // Configuration Descriptor
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, 100, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // IAD
    TUD_AUDIO_DESC_IAD(/*_firstInterface*/ 0, /*_nInterfaces*/ 0x02, /*_bFunctionClass*/ AUDIO_FUNCTION_AUDIO_CONTROL, /*_bFunctionSubClass*/ AUDIO_FUNC_SUBCLASS_AUDIOCONTROL, /*_bFunctionProtocol*/ AUDIO_FUNC_PROTOCOL_CODE_UNDEF),

    // Audio Control Interface
    TUD_AUDIO_DESC_STD_AC(/*_bInterfaceNumber*/ 0x00, /*_bInterfaceClass*/ AUDIO_CLASS_AUDIO, /*_bInterfaceSubClass*/ AUDIO_SUBCLASS_AUDIOCONTROL, /*_bInterfaceProtocol*/ AUDIO_INT_PROTOCOL_CODE_UNDEF, /*_interfaceStringIndex*/ 0x00),

    TUD_AUDIO_DESC_CS_AC(/*_baInterfaceNr*/ 0x00, /*_bInCollection*/ 0x01, /*_baInterfaceNr*/ 0x01),

    // Clock Source
    TUD_AUDIO_DESC_CLK_SRC(/*_bClockID*/ AUDIO_CLOCK_SOURCE_ID, /*_bRequestType*/ AUDIO_CS_REQUEST_AC, /*_bmControls*/ AUDIO_CLOCK_SOURCE_CONTROL_RDWR, /*_bAssocTerminal*/ 0x00, /*_iClockSource*/ 0x00),

    // Input Terminal
    TUD_AUDIO_DESC_IN_TERM(/*_bTerminalID*/ AUDIO_IN_TERM_ID, /*_wTerminalType*/ AUDIO_TERM_TYPE_USB_STREAMING, /*_bAssocTerminal*/ 0x00, /*_bCSourceID*/ AUDIO_CLOCK_SOURCE_ID, /*_bNrChannels*/ 0x02, /*_bmChannelConfig*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, /*_iChannelNames*/ 0x00, /*_bmControls*/ AUDIO_IN_TERM_CONTROL_RDWR, /*_iTerminal*/ 0x00),

    // Output Terminal
    TUD_AUDIO_DESC_OUT_TERM(/*_bTerminalID*/ AUDIO_OUT_TERM_ID, /*_wTerminalType*/ AUDIO_TERM_TYPE_OUT_SPEAKER, /*_bAssocTerminal*/ 0x00, /*_bSourceID*/ AUDIO_IN_TERM_ID, /*_bCSourceID*/ AUDIO_CLOCK_SOURCE_ID, /*_bmControls*/ AUDIO_OUT_TERM_CONTROL_RDWR, /*_iTerminal*/ 0x00),

    // Audio Streaming Interface (Alternate 0)
    TUD_AUDIO_DESC_STD_AS_INT(/*_bInterfaceNumber*/ 0x01, /*_bAlternateSetting*/ 0x00, /*_bNumEndpoints*/ 0x00, /*_bInterfaceClass*/ AUDIO_CLASS_AUDIO, /*_bInterfaceSubClass*/ AUDIO_SUBCLASS_AUDIOSTREAMING, /*_bInterfaceProtocol*/ AUDIO_INT_PROTOCOL_CODE_UNDEF, /*_iInterface*/ 0x00),

    // Audio Streaming Interface (Alternate 1)
    TUD_AUDIO_DESC_STD_AS_INT(/*_bInterfaceNumber*/ 0x01, /*_bAlternateSetting*/ 0x01, /*_bNumEndpoints*/ 0x01, /*_bInterfaceClass*/ AUDIO_CLASS_AUDIO, /*_bInterfaceSubClass*/ AUDIO_SUBCLASS_AUDIOSTREAMING, /*_bInterfaceProtocol*/ AUDIO_INT_PROTOCOL_CODE_UNDEF, /*_iInterface*/ 0x00),

    // Audio Streaming Class Specific
    TUD_AUDIO_DESC_CS_AS_INT(/*_bTerminalLink*/ AUDIO_IN_TERM_ID, /*_bmControls*/ AUDIO_AS_INTERFACE_CONTROL_RDWR, /*_bFormatType*/ AUDIO_FORMAT_TYPE_I, /*_bmFormats*/ AUDIO_DATA_FORMAT_TYPE_I_PCM, /*_bNrChannels*/ 0x02, /*_bmChannelConfig*/ AUDIO_CHANNEL_CONFIG_NON_PREDEFINED, /*_iChannelNames*/ 0x00),

    // Endpoint
    TUD_AUDIO_DESC_STD_AS_ISO_EP(/*_bEndpointAddress*/ 0x02, /*_bmAttributes*/ AUDIO_EP_TYPE_ISOCHRONOUS | AUDIO_EP_USAGE_TYPE_DATA, /*_wMaxPacketSize*/ 0x0048, /*_bInterval*/ 0x01, /*_bmRefresh*/ 0x00, /*_bSynchAddress*/ 0x00),

    // Audio Streaming Isochronous Audio Data Endpoint Descriptor
    TUD_AUDIO_DESC_CS_AS_ISO_EP(/*_bmAttributes*/ AUDIO_CS_AS_ISO_DATA_EP_ATT_NO_RESTRICT | AUDIO_CS_AS_ISO_DATA_EP_ATT_SAMPLE_FREQ, /*_bmControls*/ AUDIO_CS_AS_ISO_DATA_EP_CONTROL_RDWR, /*_bLockDelayUnits*/ AUDIO_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, /*_wLockDelay*/ 0x0000),
  };

  return desc_audio;
}

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;

  static uint16_t _desc_str[32];

  uint8_t chr_count;

  switch (index) {
    case 0:
      memcpy(&_desc_str[1], "Meshnet Audio", 13);
      chr_count = 13;
      break;

    case 1:
      memcpy(&_desc_str[1], "TinyUSB", 7);
      chr_count = 7;
      break;

    case 2:
      memcpy(&_desc_str[1], "USB Speaker", 11);
      chr_count = 11;
      break;

    default:
      return NULL;
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

  return _desc_str;
}
