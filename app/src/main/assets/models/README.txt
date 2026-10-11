把模型与着色器放在此目录下（assets/models/）。

推荐做法：完成 scripts/build_all.sh 后，直接拷贝：

    cp -r scripts/dist/arm64-v8a/res/. app/src/main/assets/models/

期望的目录结构（与上游 find_resource 的查找路径一致）：

    assets/models/
    ├── realesrgan/
    │   ├── realesr-animevideov3-x2.param / .bin
    │   ├── realesr-animevideov3-x3.param / .bin
    │   └── realesr-animevideov3-x4.param / .bin
    ├── realcugan/
    │   ├── models-se/  up2x-no-denoise.param / .bin ...
    │   ├── models-pro/ ...
    │   └── models-nose/ ...
    ├── libplacebo/
    │   ├── anime4k-v4-a.glsl
    │   ├── anime4k-v4-b.glsl
    │   └── ...
    └── rife/
        └── rife-v4.6.param / .bin ...

如果不想让 APK 变大，可以只保留实际用到的模型；
也可以不带模型，改为首次启动时下载到 filesDir/models（见 README 的“体积控制”一节）。
