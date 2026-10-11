# JNI 入口类与 native 方法名必须保留
-keepclasseswithmembernames class com.video2x.android.Video2XNative {
    native <methods>;
}
-keep class com.video2x.android.Video2XNative { *; }
-keep class com.video2x.android.AlgorithmSpec { *; }
