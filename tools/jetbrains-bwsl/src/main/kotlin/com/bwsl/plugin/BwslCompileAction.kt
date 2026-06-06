package com.bwsl.plugin

import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.CommonDataKeys
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFileManager
import java.io.File
import java.util.concurrent.TimeUnit

class BwslCompileAction : AnAction() {

    override fun getActionUpdateThread() = ActionUpdateThread.BGT

    override fun update(e: AnActionEvent) {
        val file = e.getData(CommonDataKeys.VIRTUAL_FILE)
        if (file?.extension != "bwsl" || BwslSettings.getInstance().compilerPath.isBlank()) {
            e.presentation.isEnabledAndVisible = false
            return
        }
        val hasPipeline = file.contentsToByteArray().toString(Charsets.UTF_8)
            .contains(pipelinePattern)
        e.presentation.isVisible = true
        e.presentation.isEnabled = hasPipeline
        e.presentation.description = if (hasPipeline)
            "Compile the current BWSL shader file with bwslc"
        else
            "No pipeline block found — nothing to compile"
    }

    companion object {
        private val pipelinePattern = Regex("""\bpipeline\b""")
    }

    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        val virtualFile = e.getData(CommonDataKeys.VIRTUAL_FILE) ?: return
        val settings = BwslSettings.getInstance()

        val outputDir = settings.outputDirectory.takeIf { it.isNotBlank() }
            ?: virtualFile.parent?.path
            ?: return

        val format = BwslOutputFormat.entries.firstOrNull { it.name == settings.outputFormat }
            ?: BwslOutputFormat.SPIRV_ONLY

        object : Task.Backgroundable(project, "Compiling ${virtualFile.name}", false) {
            override fun run(indicator: ProgressIndicator) {
                val cmd = mutableListOf(settings.compilerPath, virtualFile.path)
                format.flag?.let { cmd += it }
                settings.modulePaths.forEach { cmd += listOf("-modules", it) }

                val process = try {
                    ProcessBuilder(cmd)
                        .directory(File(outputDir))
                        .redirectErrorStream(true)
                        .start()
                } catch (ex: Exception) {
                    notify(project, "Failed to start compiler: ${ex.message}", NotificationType.ERROR)
                    return
                }

                val output = process.inputStream.bufferedReader().readText()
                val completed = process.waitFor(30, TimeUnit.SECONDS)
                if (!completed) {
                    process.destroy()
                    notify(project, "Compilation timed out.", NotificationType.ERROR)
                    return
                }

                if (process.exitValue() == 0) {
                    ApplicationManager.getApplication().invokeLater {
                        VirtualFileManager.getInstance().asyncRefresh(null)
                    }
                    notify(project, "${virtualFile.name} compiled successfully.", NotificationType.INFORMATION)
                } else {
                    val message = output.trim().take(1000).ifBlank { "Compilation failed (exit ${process.exitValue()})" }
                    notify(project, message, NotificationType.ERROR)
                }
            }
        }.queue()
    }

    private fun notify(project: Project, content: String, type: NotificationType) {
        ApplicationManager.getApplication().invokeLater {
            NotificationGroupManager.getInstance()
                .getNotificationGroup("BWSL Compiler")
                .createNotification(content, type)
                .notify(project)
        }
    }
}
