package com.video2x.android

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    MainScreen()
                }
            }
        }
    }
}

// --------------------------------------------------------------------------- VM

class MainViewModel(app: Application) : AndroidViewModel(app) {

    var inputUri by mutableStateOf<Uri?>(null)
        private set

    /**
     * 当前输入是否为单帧静态图片（由 setInput 判定）。
     * 图片走同一套处理管线，但编码输出为 PNG，且不适用插帧与硬件编码器。
     */
    var isImageInput by mutableStateOf(false)
        private set

    var permGranted by mutableStateOf(true)
        private set

    var algo by mutableStateOf(Algo.REALESRGAN)

    // Real-ESRGAN
    var esrganModelId by mutableStateOf(Catalogs.esrganModels.first().id)
    var esrganScale by mutableStateOf(2)

    // Real-CUGAN（组合下标）
    var cuganComboIndex by mutableStateOf(0)

    // Anime4K
    var shaderId by mutableStateOf(Catalogs.anime4kShaders.first().id)

    /** 输出倍率：目标尺寸 = 输入尺寸 × 倍率 */
    var anime4kScale by mutableStateOf(2)

    /**
     * 手动覆盖的输出宽高。留空表示按「输入分辨率 × 倍率」自动计算 ——
     * 这样竖屏输入不会被默认值写成横屏（旧版硬编码 1920x1080 会把竖屏拉变形）。
     */
    var outWidth by mutableStateOf("")
    var outHeight by mutableStateOf("")

    // RIFE
    var rifeModelId by mutableStateOf(Catalogs.rifeModels.first().id)
    var frameMultiplier by mutableStateOf(2)

    // 编码
    var codecId by mutableStateOf(Catalogs.codecs.first().id)
    var bitRateText by mutableStateOf("0")

    var ready by mutableStateOf(false)
        private set
    var prepareMessage by mutableStateOf("正在准备模型…")
        private set

    /** 最近一次任务的日志文件路径 */
    var lastLogPath by mutableStateOf("")
        private set

    val status: StateFlow<ProcessStatus> get() = ProcessBus.status

    val esrganModel: EsrganModel
        get() = Catalogs.esrganModels.firstOrNull { it.id == esrganModelId }
            ?: Catalogs.esrganModels.first()

    val cuganCombo: CuganCombo
        get() = Catalogs.cuganCombos.getOrElse(cuganComboIndex) { Catalogs.cuganCombos.first() }

    /** 当前算法实际使用的模型名（RealCUGAN 传的是 variant 目录名） */
    private fun currentModel(): String = when (algo) {
        Algo.REALESRGAN -> esrganModelId
        Algo.REALCUGAN -> cuganCombo.variant
        Algo.RIFE -> rifeModelId
        Algo.ANIME4K -> ""
    }

    /** 当前输出倍率（RIFE 不适用；Anime4K 用它推算目标尺寸） */
    private fun currentScale(): Int = when (algo) {
        Algo.REALESRGAN -> esrganScale
        Algo.REALCUGAN -> cuganCombo.scale
        Algo.ANIME4K -> anime4kScale
        else -> 0
    }

    /** Real-CUGAN 的降噪等级；其余算法传 -1 */
    private fun currentNoise(): Int = if (algo == Algo.REALCUGAN) cuganCombo.noise else -1

    init {
        checkPermission()
        prepare()
    }

    /** 刷新「所有文件访问」权限状态（从设置页返回时调用） */
    fun checkPermission() {
        permGranted = FileBridge.hasOutputPermission()
    }

    private fun prepare() {
        viewModelScope.launch {
            val ctx = getApplication<Application>()
            val msg = withContext(Dispatchers.IO) {
                try {
                    if (!ModelAssets.hasBundledModels(ctx)) {
                        return@withContext "未在 assets/models 中找到模型；请将模型放入 filesDir/models 后重启"
                    }
                    // 解压到 filesDir/models；但资源根必须传 filesDir ——
                    // 上游查找的资源名形如 "models/realesrgan/xxx.param"，
                    // 拼接后需要得到 filesDir/models/realesrgan/xxx.param
                    ModelAssets.ensureExtracted(ctx)
                    Video2XNative.setResourceDir(ctx.filesDir.absolutePath)
                    null
                } catch (t: Throwable) {
                    "模型准备失败：${t.message}"
                }
            }
            if (msg != null) prepareMessage = msg
            ready = true
        }
    }

    fun setInput(uri: Uri?) {
        inputUri = uri

        // 判断是否为「单帧静态图片」。
        // 图片与视频共用同一条 native 管线，但 UI 上要收敛可选项：
        // 不做插帧（依赖帧间关系）、不用硬件编码器（无码率/GOP 概念）。
        //
        // 显示名优先（native 侧也是按扩展名判断），MIME 仅作兜底。
        // 注意排除 gif：多帧输入会让 image2 muxer 写第二帧时失败。
        val ctx = getApplication<Application>()
        val name = uri?.let { FileBridge.displayName(ctx, it) }
        val mime = uri?.let { ctx.contentResolver.getType(it) }
        val mimeIsStaticImage = mime != null &&
            mime.startsWith("image/") && mime != "image/gif"

        isImageInput = uri != null &&
            (FileBridge.isImageName(name) || (name == null && mimeIsStaticImage))

        // 图片无法插帧 —— 若当前停在 RIFE 上，自动切回 Real-ESRGAN
        if (isImageInput && algo == Algo.RIFE) {
            algo = Algo.REALESRGAN
        }
    }

    /** 计算默认输出路径（原文件名 + 算法参数） */
    fun outputPathFor(inputName: String): String = FileBridge.buildOutputPath(
        inputName = inputName,
        algo = algo,
        model = currentModel(),
        scale = currentScale(),
        width = outWidth.toIntOrNull() ?: 0,
        height = outHeight.toIntOrNull() ?: 0,
        frameMultiplier = frameMultiplier,
        shader = shaderId,
        isImage = isImageInput,
    )

    fun start(outPath: String) {
        if (ProcessBus.status.value.running) return
        val ctx = getApplication<Application>()
        val inU = inputUri ?: return

        // 把日志切到文本文件：/storage/emulated/0/Video2x/log/video2x_<时间戳>.log
        val logPath = FileBridge.newLogFilePath()
        lastLogPath = logPath
        if (!Video2XNative.setLogFile(logPath)) {
            prepareMessage = "日志文件创建失败：$logPath（请检查「所有文件访问」权限）"
        }

        ProcessBus.request = ProcessRequest(
            inUri = inU.toString(),
            outPath = outPath,
            logPath = logPath,
            algo = algo,
            model = currentModel(),
            scale = currentScale(),
            noiseLevel = currentNoise(),
            width = outWidth.toIntOrNull() ?: 0,
            height = outHeight.toIntOrNull() ?: 0,
            frameMultiplier = frameMultiplier,
            shader = shaderId,
            codec = codecId,
            bitRate = bitRateText.toLongOrNull() ?: 0L,
            isImage = isImageInput,
        )
        ProcessBus.status.value = ProcessStatus(running = true, message = "启动中…")

        ContextCompat.startForegroundService(
            ctx,
            Intent(ctx, ProcessingService::class.java),
        )
    }

    fun abort() {
        val ctx = getApplication<Application>()
        ctx.startService(
            Intent(ctx, ProcessingService::class.java)
                .setAction(ProcessingService.ACTION_ABORT),
        )
    }
}

// -------------------------------------------------------------------------- UI

@Composable
private fun MainScreen(vm: MainViewModel = viewModel()) {
    val context = LocalContext.current
    val status by vm.status.collectAsState()

    // 从「设置 → 所有文件访问」返回时刷新权限状态
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) vm.checkPermission()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    // 这里用 StartActivityForResult 而不是 OpenDocument 合约：
    // OpenDocument 只会拉起系统的 DocumentsUI（SAF），相册、MT管理器等
    // 第三方文件管理器根本不会出现在候选里。改成 ACTION_GET_CONTENT
    // 并显式包一层 createChooser，系统会弹出「打开方式」列表任选。
    val inputPicker = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == android.app.Activity.RESULT_OK) {
            val uri = result.data?.data
            if (uri != null) {
                try {
                    // 只有 SAF 来源的 URI 才有可持久化权限；相册返回的普通
                    // content URI 会抛异常，忽略即可 —— importInput 会立刻
                    // 拷贝到私有目录，不依赖长期授权。
                    context.contentResolver.takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: Exception) {
                }
                vm.setInput(uri)
            }
        }
    }

    /** 备选入口：直接走系统文档选择器（SAF），某些设备上更稳 */
    fun pickVideoSystem() {
        try {
            inputPicker.launch(
                Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    // 同时允许图片与视频：EXTRA_MIME_TYPES 让 SAF 只列出这两类，
                    // type 保持 */* 才能让多类型过滤生效。
                    type = "*/*"
                    putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("image/*", "video/*"))
                    addCategory(Intent.CATEGORY_OPENABLE)
                }
            )
        } catch (_: Exception) {
            // 两种方式都不可用
        }
    }

    // 注意：Kotlin 的局部函数不支持前向引用，故 pickVideoSystem 必须定义在其上。
    fun pickVideo() {
        // 关键：MIME 用 "*/*"，不要用 "video/*"。
        //
        // intent 解析要求「发送方的 MIME 必须落在接收方声明的类型范围内」。
        // 很多第三方文件管理器（如 MT 管理器）的 intent-filter 里只声明了
        // */* 或若干具体类型，并没有声明 video/* —— 发 video/* 时它们反而不
        // 被匹配，不会出现在候选列表里。
        //
        // 参照 MikuPlay：其插件的贴图上传用 <input type="file">（未设 accept），
        // 宿主 Capacitor 据此发出 */* + CATEGORY_OPENABLE 的 GET_CONTENT，
        // 实测能正常弹出包含第三方文件管理器的选择窗口。这里对齐它的做法。
        val inner = Intent(Intent.ACTION_GET_CONTENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        try {
            inputPicker.launch(Intent.createChooser(inner, "选择图片或视频"))
        } catch (_: Exception) {
            pickVideoSystem()
        }
    }

    val inputName = vm.inputUri?.let { FileBridge.displayName(context, it) } ?: "input.mp4"

    // 输出路径随参数变化自动更新
    val outputPath = remember(
        vm.inputUri, vm.algo, vm.esrganModelId, vm.esrganScale, vm.cuganComboIndex,
        vm.rifeModelId, vm.frameMultiplier, vm.shaderId, vm.outWidth, vm.outHeight,
        vm.isImageInput,
    ) {
        vm.outputPathFor(inputName)
    }

    // RIFE 模型是按需导入的，进出设置页都要重新扫描一次
    var showSettings by remember { mutableStateOf(false) }
    var rifeInstalled by remember { mutableStateOf(RifeModels.installed(context)) }

    // 已导入的模型与当前选择不一致时（刚导入、或换了一批模型），自动切到第一个
    LaunchedEffect(rifeInstalled) {
        if (rifeInstalled.isNotEmpty() && vm.rifeModelId !in rifeInstalled) {
            vm.rifeModelId = rifeInstalled.first()
        }
    }

    if (showSettings) {
        SettingsScreen(
            installed = rifeInstalled,
            onImported = { rifeInstalled = RifeModels.installed(context) },
            onBack = { showSettings = false },
        )
        return
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "Video2X",
                style = MaterialTheme.typography.headlineSmall,
                modifier = Modifier.weight(1f),
            )
            OutlinedButton(onClick = { showSettings = true }) { Text("设置") }
        }
        Text(
            "视频超分 / 插帧（基于 libvideo2x + FFmpeg + ncnn/Vulkan）",
            style = MaterialTheme.typography.bodySmall,
        )

        if (!NativeState.ready) {
            Text(
                "native 初始化失败：${NativeState.error ?: "未知"}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }

        HorizontalDivider()

        // ---- 输入 ----
        Column {
            Text(
                if (vm.isImageInput) "输入图片" else "输入视频",
                style = MaterialTheme.typography.labelLarge,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Button(onClick = { pickVideo() }) {
                    Text("选择")
                }
                Spacer(Modifier.width(8.dp))
                OutlinedButton(onClick = { pickVideoSystem() }) {
                    Text("系统文件")
                }
                Spacer(Modifier.width(8.dp))
                Text(inputName, style = MaterialTheme.typography.bodySmall)
            }
            Text(
                "「选择」会弹出应用列表，可选相册、第三方文件管理器（如 MT 管理器）等；" +
                    "「系统文件」直接调用系统文档选择器。视频与图片都可以选。",
                style = MaterialTheme.typography.bodySmall,
            )
        }

        // ---- 权限 ----
        if (!vm.permGranted) {
            Column {
                Text(
                    "需要「所有文件访问」权限才能输出到 ${FileBridge.OUTPUT_DIR}/",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
                OutlinedButton(onClick = {
                    FileBridge.permissionIntent(context)?.let { context.startActivity(it) }
                }) {
                    Text("去授权")
                }
            }
        }

        HorizontalDivider()

        // ---- 算法 ----
        // 图片输入不支持插帧（RIFE 依赖帧间关系），从候选里去掉
        EnumDropdown(
            label = "算法",
            options = if (vm.isImageInput) {
                Algo.entries.filter { it != Algo.RIFE }
            } else {
                Algo.entries.toList()
            },
            selected = vm.algo,
            optionLabel = { it.label },
            onSelect = { vm.algo = it },
        )

        when (vm.algo) {
            Algo.ANIME4K -> {
                EnumDropdown(
                    label = "着色器",
                    options = Catalogs.anime4kShaders,
                    selected = Catalogs.anime4kShaders.firstOrNull { it.id == vm.shaderId }
                        ?: Catalogs.anime4kShaders.first(),
                    optionLabel = { it.label },
                    onSelect = { vm.shaderId = it.id },
                )
                IntDropdown("输出倍率", listOf(1, 2, 3, 4), vm.anime4kScale) { vm.anime4kScale = it }

                // 目标尺寸默认按「输入分辨率 × 倍率」自动计算；
                // 仅当需要非整数倍的尺寸时才手填（宽高需同时填写才生效）。
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = vm.outWidth,
                        onValueChange = { vm.outWidth = it.filter(Char::isDigit) },
                        label = { Text("手动宽（可选）") },
                        modifier = Modifier.width(150.dp),
                        singleLine = true,
                    )
                    Spacer(Modifier.width(12.dp))
                    OutlinedTextField(
                        value = vm.outHeight,
                        onValueChange = { vm.outHeight = it.filter(Char::isDigit) },
                        label = { Text("手动高（可选）") },
                        modifier = Modifier.width(150.dp),
                        singleLine = true,
                    )
                }
                Text(
                    "留空 = 按「输入分辨率 × 倍率」自动计算（推荐）；填写则按填写尺寸输出。",
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            Algo.REALESRGAN -> {
                EnumDropdown(
                    label = "模型",
                    options = Catalogs.esrganModels,
                    selected = vm.esrganModel,
                    optionLabel = { it.label },
                    onSelect = { m ->
                        vm.esrganModelId = m.id
                        // 新模型可能不支持当前倍率，回退到它的第一个可用倍率
                        if (vm.esrganScale !in m.scales) vm.esrganScale = m.scales.first()
                    },
                )
                IntDropdown("超分倍率", vm.esrganModel.scales, vm.esrganScale) { vm.esrganScale = it }
            }

            Algo.REALCUGAN -> {
                EnumDropdown(
                    label = "模型组合（变体 · 倍率 · 降噪）",
                    options = Catalogs.cuganCombos,
                    selected = vm.cuganCombo,
                    optionLabel = { it.label },
                    onSelect = { vm.cuganComboIndex = Catalogs.cuganCombos.indexOf(it) },
                )
            }

            Algo.RIFE -> {
                if (rifeInstalled.isEmpty()) {
                    Text(
                        "尚未导入补帧模型，请到右上角「设置」页导入。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                } else {
                    EnumDropdown(
                        label = "模型",
                        options = rifeInstalled,
                        selected = rifeInstalled.firstOrNull { it == vm.rifeModelId }
                            ?: rifeInstalled.first(),
                        optionLabel = { it },
                        onSelect = { vm.rifeModelId = it },
                    )
                }
                IntDropdown("插帧倍数", Catalogs.frameMultipliers, vm.frameMultiplier) {
                    vm.frameMultiplier = it
                }
            }
        }

        // 图片输出为 PNG（无损），没有编码器与码率的概念 —— 整块隐藏
        if (!vm.isImageInput) {
            EnumDropdown(
                label = "视频编码器",
                options = Catalogs.codecs,
                selected = Catalogs.codecs.firstOrNull { it.id == vm.codecId }
                    ?: Catalogs.codecs.first(),
                optionLabel = { it.label },
                onSelect = { vm.codecId = it.id },
            )

            OutlinedTextField(
                value = vm.bitRateText,
                onValueChange = { vm.bitRateText = it.filter(Char::isDigit) },
                label = { Text("码率（bit/s，0 = 自动）") },
                singleLine = true,
            )
            Text(
                "0 = 自动：按「输入码率 × 输出/输入像素比」推算（硬件编码需正码率）；" +
                    "软编码 libx264 填 0 时走默认 CRF。",
                style = MaterialTheme.typography.bodySmall,
            )
        } else {
            Text(
                "图片模式：输出为 PNG（无损），不需要选择编码器与码率。" +
                    "支持 png / jpg / webp / bmp / tiff；GIF 等多帧格式不支持。",
                style = MaterialTheme.typography.bodySmall,
            )
        }

        HorizontalDivider()

        // ---- 输出（自动生成） ----
        Column {
            Text("输出文件", style = MaterialTheme.typography.labelLarge)
            Text(
                outputPath,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
        }

        HorizontalDivider()

        // ---- 执行 ----
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = { vm.start(outputPath) },
                enabled = vm.ready && vm.permGranted && !status.running && vm.inputUri != null
                    && NativeState.ready,
            ) { Text("开始") }

            OutlinedButton(
                onClick = { vm.abort() },
                enabled = status.running,
            ) { Text("中止") }
        }

        if (status.running || status.done) {
            LinearProgressIndicator(
                progress = { status.progress },
                modifier = Modifier.fillMaxWidth(),
            )
            Text(status.message, style = MaterialTheme.typography.bodyMedium)
        }

        if (!vm.ready) {
            Text(vm.prepareMessage, style = MaterialTheme.typography.bodySmall)
        }

        Text(
            if (vm.lastLogPath.isNotEmpty()) {
                "本次日志：${vm.lastLogPath}"
            } else {
                "日志目录：${FileBridge.LOG_DIR}"
            },
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
        )
    }
}

// ------------------------------------------------------------------ 复用组件

@Composable
private fun <T> EnumDropdown(
    label: String,
    options: List<T>,
    selected: T,
    optionLabel: (T) -> String,
    onSelect: (T) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Box {
            OutlinedButton(onClick = { expanded = true }) {
                Text(optionLabel(selected))
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { opt ->
                    DropdownMenuItem(
                        text = { Text(optionLabel(opt)) },
                        onClick = {
                            onSelect(opt)
                            expanded = false
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun IntDropdown(
    label: String,
    options: List<Int>,
    selected: Int,
    onSelect: (Int) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        Box {
            OutlinedButton(onClick = { expanded = true }) { Text("x$selected") }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { opt ->
                    DropdownMenuItem(
                        text = { Text("x$opt") },
                        onClick = {
                            onSelect(opt)
                            expanded = false
                        },
                    )
                }
            }
        }
    }
}
