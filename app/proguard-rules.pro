# R8 / ProGuard 规则
#
# 这个 App 的 Kotlin/Java 代码几乎全是直接调用（没有反射、没有序列化框架），
# 所以需要保留的东西很少 —— 核心只有 JNI 入口。

# ------------------------------------------------------------------ JNI
#
# JNI 函数名是 Java_com_video2x_android_Video2XNative_process 这种形式，
# 依赖「完整类名 + 方法名」。类名被混淆后 native 侧就找不到方法了，
# 因此这个类必须整体保留（且不能被改名）。
-keep class com.video2x.android.Video2XNative { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}

# ------------------------------------------------- 模型与算法的元数据
#
# Catalogs 里的这些数据类的实例会被 Compose 读取，未被反射访问，
# 理论上可以混淆；但它们同时被 Kotlin 枚举/列表推导引用，
# 保留名字便于日后看崩溃堆栈时定位，代价很小。
-keep class com.video2x.android.AlgorithmSpec { *; }

# ------------------------------------------------------------ ViewModel
#
# MainViewModel 由 androidx 的 ViewModelProvider 反射实例化
# （modelClass.getConstructor(Application::class.java)）。
# R8 的反射分析一般能识别这种调用并保留构造函数，但它是 App 的入口对象 ——
# 一旦构造失败就是启动即崩。这里显式保留构造函数，代价只有几十字节。
# 类名仍允许混淆（调用方持有的是 Class 对象，不依赖名字）。
-keepclassmembers class * extends androidx.lifecycle.ViewModel {
    <init>(...);
}

# Manifest 中声明的组件（MainActivity / ProcessingService / Video2XApp）
# 由 R8 自动保留 —— 它会读取 AndroidManifest 并同步类名，无需手写规则。

# ---------------------------------------------------------------- 调试
#
# 保留行号（隐藏源文件名），这样 release 包的崩溃堆栈仍然可读。
# 体积开销可以忽略。
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# 保留注解信息（Compose 与 Kotlin 元数据会用到）
-keepattributes *Annotation*, InnerClasses, Signature, Exceptions
