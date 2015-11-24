# Crucible
libobs-based capture system for Forge

## Building
Requires libobs from [obs-studio](https://github.com/ForgeGaming/obs-studio-internal) and [boost](http://www.boost.org) with the following environment variables set

- __$(Boost)__ - path to boost base dir
- __$(OBSLib)__ - path to libobs for includes eg: /obs-studio/libobs
- __$(OBSBuild)__ - path to libobs build for library files eg: /obs-studio/build32/libobs

## Debugging
Should work out of the box with [obs-studio-internal@75622cc](https://github.com/ForgeGaming/obs-studio-internal/commit/75622cc147f6c73f19355f7d92f349bab208b489) or later (no need to copy any files, as long as the libobs rundir is set up properly (can be checked by building and running the OBS UI))

## Running
Requires modules and data from obs-studio in the same directory you're running from (this will change in future)
