模型与着色器目录（打包进 APK 的 assets/models/）。

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
        ├── rife-v4/flownet.param / .bin
        ├── rife-v4.25/flownet.param / .bin
        ├── rife-v4.25-lite/flownet.param / .bin
        ├── rife-v4.26/flownet.param / .bin
        └── rife-v4.6/flownet.param / .bin

几点注意：

- 本目录的内容会被**完整打包进 APK**（当前约 155 MB），首次启动时解压到
  filesDir/models。APK 体积的大头就在这里 —— 想显著减少只能从此处取舍。

- .gitignore 排除了本目录下除本文件之外的所有内容：模型不入库，
  clone 之后需要自行运行 scripts/fetch_sources.sh + build_all.sh 获取。

- 增删模型后，记得递增 ModelAssets.kt 里的 VERSION，否则已安装的设备
  不会重新解压。

- 内置的是 RIFE v4 系列 5 个模型。上游其余版本（v2 / v3 / HD / UHD / anime）
  可以在 App 内「设置 → 补帧模型」按需导入，不必打进 APK。
