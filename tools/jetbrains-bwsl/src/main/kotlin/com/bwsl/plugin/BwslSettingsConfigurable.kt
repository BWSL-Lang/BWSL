package com.bwsl.plugin

import com.intellij.openapi.fileChooser.FileChooserDescriptor
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.ui.ComboBox
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.SimpleListCellRenderer
import com.intellij.ui.ToolbarDecorator
import com.intellij.ui.components.JBList
import com.intellij.ui.dsl.builder.Align
import com.intellij.ui.dsl.builder.bindText
import com.intellij.ui.dsl.builder.panel
import javax.swing.DefaultListModel
import javax.swing.JComponent

class BwslSettingsConfigurable : Configurable {

    private var panel: DialogPanel? = null
    private val moduleListModel = DefaultListModel<String>()
    private var formatCombo: ComboBox<BwslOutputFormat>? = null

    override fun getDisplayName(): String = "BWSL"

    override fun createComponent(): JComponent {
        val settings = BwslSettings.getInstance()
        moduleListModel.clear()
        settings.modulePaths.forEach { moduleListModel.addElement(it) }

        formatCombo = ComboBox(BwslOutputFormat.entries.toTypedArray()).apply {
            renderer = SimpleListCellRenderer.create("") { it.displayName }
            selectedItem = BwslOutputFormat.entries.firstOrNull { it.name == settings.outputFormat }
                ?: BwslOutputFormat.SPIRV_ONLY
        }

        val moduleList = JBList(moduleListModel)
        val decorator = ToolbarDecorator.createDecorator(moduleList)
            .setAddAction {
                val project = com.intellij.openapi.project.ProjectManager.getInstance().openProjects.firstOrNull()
                val initialDir = project?.basePath?.let {
                    com.intellij.openapi.vfs.LocalFileSystem.getInstance().findFileByPath(it)
                }
                val descriptor = FileChooserDescriptor(true, true, false, false, false, false)
                    .withTitle("Select Module Directory")
                val chosen = com.intellij.openapi.fileChooser.FileChooser.chooseFile(descriptor, project, initialDir)
                if (chosen != null) {
                    val dir = if (chosen.isDirectory) chosen else chosen.parent
                    if (dir != null && !moduleListModel.elements().toList().contains(dir.path)) {
                        moduleListModel.addElement(dir.path)
                    }
                }
            }
            .setRemoveAction { moduleList.selectedValuesList.forEach { moduleListModel.removeElement(it) } }
            .createPanel()

        val project = com.intellij.openapi.project.ProjectManager.getInstance().openProjects.firstOrNull()
        return panel {
            row("Compiler path:") {
                textFieldWithBrowseButton(
                    FileChooserDescriptor(true, false, false, false, false, false)
                        .withTitle("Select BWSL Compiler"),
                    project
                ).bindText(settings::compilerPath)
            }
            row("Output format:") {
                cell(formatCombo!!)
            }
            row("Output directory:") {
                textFieldWithBrowseButton(
                    FileChooserDescriptor(false, true, false, false, false, false)
                        .withTitle("Select Output Directory"),
                    project
                ).bindText(settings::outputDirectory)
                    .comment("Defaults to the source file's directory if empty")
            }
            row("Module paths:") {}
            row {
                cell(decorator).align(Align.FILL)
            }.resizableRow()
        }.also { panel = it }
    }

    override fun isModified(): Boolean {
        if (panel?.isModified() == true) return true
        val settings = BwslSettings.getInstance()
        val currentFormat = (formatCombo?.selectedItem as? BwslOutputFormat)?.name ?: BwslOutputFormat.SPIRV_ONLY.name
        if (currentFormat != settings.outputFormat) return true
        return settings.modulePaths != moduleListModel.elements().toList()
    }

    override fun apply() {
        panel?.apply()
        val settings = BwslSettings.getInstance()
        settings.outputFormat = (formatCombo?.selectedItem as? BwslOutputFormat)?.name
            ?: BwslOutputFormat.SPIRV_ONLY.name
        settings.modulePaths = moduleListModel.elements().toList().toMutableList()
        com.intellij.openapi.project.ProjectManager.getInstance().openProjects.forEach { project ->
            com.intellij.codeInsight.daemon.DaemonCodeAnalyzer.getInstance(project).restart("BWSL settings changed")
        }
    }

    override fun reset() {
        panel?.reset()
        val settings = BwslSettings.getInstance()
        formatCombo?.selectedItem = BwslOutputFormat.entries.firstOrNull { it.name == settings.outputFormat }
            ?: BwslOutputFormat.SPIRV_ONLY
        moduleListModel.clear()
        settings.modulePaths.forEach { moduleListModel.addElement(it) }
    }

    override fun disposeUIResources() {
        panel = null
        formatCombo = null
    }
}
