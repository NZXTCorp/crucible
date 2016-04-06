# Crucible
libobs-based capture system for Forge

## Building
Requires libobs from [obs-studio](https://github.com/ForgeGaming/obs-studio-internal) and [boost](http://www.boost.org) with the following environment variables set

- __$(Boost)__ - path to boost base dir
- __$(OBSLib)__ - path to libobs for includes eg: /obs-studio/libobs
- __$(OBSBuild)__ - path to libobs build for library files eg: /obs-studio/build32/libobs
- __$(OBSBuild64)__ - path to libobs build for 64 bit library files eg: /obs-studio/build64/libobs
- __$(ForgeLib)__ - path to a 'forge libraries' collection that contains cajun
- __$(MinHook)__ - path to [MinHook](https://github.com/TsudaKageyu/minhook) repository (VC12 solution should be compiled for all targets)

## Running
Requires modules and data from obs-studio in the same directory you're running from (this will change in future)
