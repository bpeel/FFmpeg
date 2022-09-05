Flootay FFmpeg
==============

This is a fork of [FFmpeg](https://ffmpeg.org/) that just adds a video filter to overlay a [flootay](https://github.com/bpeel/flootay) script onto a video.

In order to enable the flootay filter, make sure you first install the flootay library somewhere so that the FFmpeg configure script can find the pkg-config file. Then enable the dependencies when you configure ffmpeg with a command like this:

```bash
./configure --enable-flootay --enable-cairo
```

Once FFmpeg is built you can use the filter like this:

```bash
ffmpeg -i my-input-video.mp4 \
       -vf flootay=filename=overlay.flt \
       my-output-video.mp4
```

There is some [documentation](https://github.com/bpeel/flootay/blob/main/README.md#flootay-language) about the flootay language in the flootay repo.
