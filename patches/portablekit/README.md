# PortableKit changes

The Infected port builds against [PortableKit](https://github.com/TeamGDB/PortableKit) (MIT) with the changes in this folder:
73 patches in order, written for upstream commit `33800c4060c53f3233e08f8ab2cc4590a0872063`. `scripts/get-portablekit.sh`
fetches that commit and applies them, and checks that the result is the tree the port was built from.

They are the part of the work that lives in the framework instead of the game's own repository: the coroutine kernel for the
Vita's stack rules, the OpenGL ES renderer and its vitaGL-specific paths (split walker and GL threads, frame skipping,
specialised shaders, compact vertices, HUD upscale, shader pre-linking), HD texture and HD movie replacement, the settings
menu, bring-up and measurement tools, and the fast texture decoders. Each patch has the commit message it had when it was
written. A merge of several branches is one patch that lists what it brought in.

All of it is MIT, like PortableKit, and was written by Claude (see the README of this repository).
