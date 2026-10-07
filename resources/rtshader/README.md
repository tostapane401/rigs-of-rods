# RTShaderLib (OGRE 1.11.6)

Copied verbatim from `OGRECave/ogre` tag `v1.11.6`, folder `Samples/Media/RTShaderLib/HLSL_Cg`
(MIT license, see headers). These files are consumed by the RTShader System when the
render system has no fixed-function pipeline (Direct3D11 / Xbox UWP).

Do NOT mix with RTShaderLib files from other OGRE versions: the generator of 1.11.6
emits calls such as `FFP_Alpha_Test`, `FFP_Normalize`, `FFP_PixelFog_PositionDepth`
that only exist in this exact library revision. Do not add `*.hlsl` copies either:
the HLSL writer prefers `<lib>.hlsl` from the default group and would bypass these.
