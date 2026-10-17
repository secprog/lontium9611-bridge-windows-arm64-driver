# Acknowledgements and notices

Thanks to [WOA-Project's LT9611 driver](https://github.com/WOA-Project/windows_hardware_bridge_lt9611_src/),
on which the original bridge and SPB support is based, and to Microsoft,
Bingxing Wang, LumiaWoA and DuoWoA contributors.

Special thanks to [Gustave Monce (gus33000)](https://github.com/gus33000) for
[publishing the original LT9611 Windows driver](https://github.com/WOA-Project/windows_hardware_bridge_lt9611_src/).

## Project license

Original project contributions are licensed under `GPL-2.0-only`; see
[LICENSE](LICENSE). Retained third-party material keeps its existing terms.

## Linux hardware references

Thanks to the Linux kernel contributors for the following hardware references:

- [LT9611 bridge](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/bridge/lontium-lt9611.c):
  `GPL-2.0` (the kernel's identifier for GPL version 2 only).
  Copyright (c) 2018, The Linux Foundation. All rights reserved.
  Copyright (c) 2019-2020. Linaro Limited.
- [SC7280 LPASS pin-controller](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-sc7280-lpass-lpi.c):
  `GPL-2.0-only`.
  Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
- [LPASS pin-controller core](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-lpass-lpi.c)
  and [header](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-lpass-lpi.h):
  `GPL-2.0-only`.
  Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
  Copyright (c) 2020 Linaro Ltd.

These references apply to LT9611 register programming in `OEMBridge/Device.c`
and LPASS pad configuration in `OEMBridge/LpiMux.c` and `OEMBridge/LpiMux.h`.

## Pulse-Eight / libCEC protocol reference

Thanks to [Pulse-Eight](https://github.com/Pulse-Eight) for the great
[libCEC](https://github.com/Pulse-Eight/libcec) library and their work on
HDMI-CEC. libCEC also provided the adapter protocol reference. The
[USB adapter message definitions](https://github.com/Pulse-Eight/libcec/blob/master/src/libcec/adapter/Pulse-Eight/USBCECAdapterMessage.h)
and [CEC type definitions](https://github.com/Pulse-Eight/libcec/blob/master/include/cectypes.h)
are offered under `GPL-2.0-or-later`, alternatively under a commercial license
from Pulse-Eight. See [libCEC's license](https://github.com/Pulse-Eight/libcec/blob/master/LICENSE.md).

libCEC(R) is Copyright (C) 2011-2026 Pulse-Eight Limited. All rights reserved.
libCEC(R) is a trademark of Pulse-Eight Limited.

The protocol reference applies to `P8CecUde/P8Cec.h`,
`P8CecUde/P8Protocol.c` and `P8CecUde/UsbDevice.c`.

The GPL version 2 text is retained in [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).
These entries identify the referenced projects' licenses.

## SPB notices

Copyright (c) Microsoft Corporation. All Rights Reserved.
Copyright (c) Bingxing Wang. All Rights Reserved.
Copyright (c) LumiaWoA authors. All Rights Reserved.
Copyright (c) DuoWoA authors. All Rights Reserved.

`OEMBridge/Spb.c` carries the upstream BSD-3-Clause designation. Its license
text is retained in [LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt).
The Microsoft copyright notice for `OEMBridge/Spb.h` is retained above.
