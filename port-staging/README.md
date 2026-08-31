# port-staging

A verbatim copy of `MicroWriter-BASIC-PaperS3`'s `editor/src/` and
`editor/lib/`, at the commit this repository was created from.

**Nothing here is edited.** Files move out of here into `editor/src/` or
`editor/lib/` as they are ported, and the port happens on the copy. Keeping
this side pristine means a bug that appears during porting can always be
diffed against the version known to work on the other device, which is the
single most useful thing to have when a port misbehaves.

`lib/TinyBasic/` is absent on purpose: it is fetched at build time by
`patches/tinybasic/fetch.sh` and is never versioned.

What is here that is not coming across at all, and why, is in
`docs/PORTING_PLAN.md` under "What is not coming across".
