# module-sndfile

A channel driver module for [Yate](https://yate.null.ro) to read and write
audio files using [libsndfile](http://www.mega-nerd.com/libsndfile/).

## How to compile

Use the Makefile provided. Proper build system support is still a TODO

The output module will be called `sndfile.yate`.

## How to use

Usage is very similar to the [Wavefile](http://docs.yate.ro/wiki/Wavefile)
module included in the standard Yate distribution, only better because it
actually supports, among others, WAV files.

module-sndfile uses the prefix of 'snd/' for its channels and message addresses.

## Todo

More to come.
