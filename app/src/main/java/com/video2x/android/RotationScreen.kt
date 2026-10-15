package com.video2x.android

import android.app.Activity
import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material3.Checkbox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

/**
 * 视频旋转 / 翻转二级页面（从右上角三点菜单进入）。
 *
 * 旋转由纯软件滤镜（libavfilter 的 transpose / hflip / vflip）完成，
 * 不经过 libplacebo / Vulkan，因此不会触发横屏超分场景下的 GPU 卡死。
 *
 * 系统返回键（BackHandler）会回到主界面，而不是直接退出 App。
 */
@Composable
fun RotationScreen(onBack: () -> Unit) {
    val vm: MainViewModel = viewModel()
    val context = LocalContext.current
    val status by vm.status.collectAsState()

    // 系统返回键：回到主界面，而不是直接退出 App
    BackHandler { onBack() }

    val inputName = vm.inputUri?.let { FileBridge.displayName(context, it) } ?: "input.mp4"
    val rotatePath = androidx.compose.runtime.remember(vm.inputUri, vm.rotateTransform) {
        vm.rotateOutputPath(inputName)
    }

    // 输入选择器（与主页一致：弹出应用列表，可选相册 / 第三方文件管理器）
    val inputPicker = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            val uri: Uri? = result.data?.data
            if (uri != null) {
                try {
                    context.contentResolver.takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: Exception) {
                    // 非 SAF 来源的普通 content URI 会抛异常，忽略即可
                }
                vm.setInput(uri)
            }
        }
    }

    fun pickInput() {
        val inner = Intent(Intent.ACTION_GET_CONTENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        try {
            inputPicker.launch(Intent.createChooser(inner, "选择视频"))
        } catch (_: Exception) {
            // 没有可用的文件选择器
        }
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
                "视频旋转",
                style = MaterialTheme.typography.headlineSmall,
                modifier = Modifier.weight(1f),
            )
            OutlinedButton(onClick = onBack) { Text("返回") }
        }

        HorizontalDivider()

        // ---- 输入 ----
        Text("输入视频", style = MaterialTheme.typography.labelLarge)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Button(onClick = { pickInput() }) { Text("选择") }
            Spacer(Modifier.width(8.dp))
            Text(inputName, style = MaterialTheme.typography.bodySmall)
        }
        Text(
            "「选择」会弹出应用列表，可选相册、第三方文件管理器（如 MT 管理器）等。",
            style = MaterialTheme.typography.bodySmall,
        )

        HorizontalDivider()

        // ---- 变换（位掩码，可多选组合） ----
        Text("变换（可多选组合）", style = MaterialTheme.typography.labelLarge)
        val options = listOf(
            "顺时针旋转 90°" to 1,
            "逆时针旋转 90°" to 2,
            "水平翻转" to 4,
            "垂直翻转" to 8,
        )
        options.forEach { (label, bit) ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(
                    checked = vm.rotateTransform and bit != 0,
                    onCheckedChange = {
                        vm.rotateTransform = if (it) {
                            vm.rotateTransform or bit
                        } else {
                            vm.rotateTransform and bit.inv()
                        }
                    },
                )
                Text(label)
            }
        }
        Text(
            "旋转 / 翻转由纯软件滤镜（libavfilter）完成，不经过 GPU，" +
                "因此不会触发超分时遇到的横屏卡死问题。",
            style = MaterialTheme.typography.bodySmall,
        )

        HorizontalDivider()

        // ---- 输出（随变换自动变化） ----
        Text("输出文件", style = MaterialTheme.typography.labelLarge)
        Text(
            rotatePath,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
        )

        HorizontalDivider()

        // ---- 执行 ----
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = { vm.startRotate(rotatePath) },
                enabled = vm.permGranted && !status.running && vm.inputUri != null
                    && vm.rotateTransform != 0,
            ) { Text("开始旋转") }

            OutlinedButton(
                onClick = { vm.abort() },
                enabled = status.running,
            ) { Text("中止") }
        }

        if (status.running || status.done) {
            if (status.running) {
                LinearProgressIndicator(
                    progress = { status.progress },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            Text(status.message, style = MaterialTheme.typography.bodyMedium)
        }
    }
}
