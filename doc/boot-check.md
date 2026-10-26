# Palm TX boot smoke test

Build with `make`, then run from the repository root with your own firmware:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./out/frond \
  --experimental-tx /path/to/Palm-TX-5.4-nor.bin \
  --nand /path/to/Palm-TX-5.4-nand.bin --no-sound --run-for 60 \
  --framebuffer-output /tmp/frond-tx-setup.png
```

Inspect the resulting image. A zero exit status or `LCD=1` only means bounded
execution completed; check that the guest reached a usable screen. A fresh TX
starts at Date & Time setup:

![Palm TX Date and Time setup](images/tx-setup.png)

Boot duration depends on host load and CPU throughput. Allow more time if the
capture still shows the splash screen. Dummy SDL drivers can test execution and
capture the framebuffer, but cannot test audible playback.

The required firmware layout is described in [native core notes](tx-native.md).
The original firmware files are read into memory and are not overwritten. For
stylus replay and calibration, see [input testing](input-testing.md).
