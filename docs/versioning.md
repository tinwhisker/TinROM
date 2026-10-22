# Version and build numbers

`0.1.0-9B84A47C`

The left half is the project version, set in `project()` in `CMakeLists.txt`
and reachable as `TINROM_VERSION_STRING`. The right half identifies the
individual build.

## The build number

The local build time as `YYMMDDHHMM`, read as a decimal number and written in
hex:

```
  26-09-16 13:40  ->  2609161340  ->  9B84A47C
```

Eight characters, short enough to read out over the phone, and ordered - a
later build always sorts after an earlier one, as text or as a number. When a
bug report says `0.1.0-9B84A47C`, that is one specific image.

It is available four ways, all declared in `tinrom/version.h`:

| Symbol | Example | For |
| --- | --- | --- |
| `tinromVersionFull` | `0.1.0-9B84A47C` | anywhere the version is shown |
| `tinromBuildNumber` | `9B84A47C` | the build alone |
| `tinromBuildTime` | `26-09-16 13:40` | people, rather than logs |
| `tinromBuildStamp` | `2609161340` | comparing two builds numerically |

It appears in the boot log, the main menu title, the about screen and the
system information screen.

## How it is generated

`cmake/buildstamp.cmake`, run with `cmake -P` as part of the build. No Python
and no other tool - it runs on the cmake that is already building the project.

Two decisions in there are worth knowing about:

**It writes a `.c` file, not a header.** The value changes every build, and a
header that changes every build rebuilds everything that includes it -
`version.h` is included nearly everywhere. As a generated source file, one
object is recompiled and the link redone.

**It runs every build, not at configure time.** The custom command lists an
`OUTPUT` that is never created, which is what keeps it perpetually out of
date. A stamp fixed at configure time would identify the last time you ran
cmake, which is not the question anyone is asking. The script then only
rewrites the file when the value has actually changed, so two builds in the
same minute do not relink for nothing.

## Reproducible builds

A timestamp makes a build non-reproducible by design. To pin it:

```
cmake --preset debug -DTINROM_BUILD_STAMP=2609161340
```

The configure summary says which mode you are in.

## After 2042

`YYMMDDHHMM` passes 2^32 in 2043. The text form simply grows a ninth hex
digit and keeps working; `tinromBuildStamp`, being a `uint32_t`, wraps. So the
number is the thing to stop trusting then, not the string. Noted here rather
than fixed, because widening it now costs four bytes in every image for a
problem seventeen years out.
