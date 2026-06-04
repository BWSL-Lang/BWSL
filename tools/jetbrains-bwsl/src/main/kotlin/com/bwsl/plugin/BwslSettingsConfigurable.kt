package com.bwsl.plugin

import com.intellij.openapi.fileChooser.FileChooserDescriptor
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.dsl.builder.bindText
import com.intellij.ui.dsl.builder.panel
import javax.swing.JComponent

class BwslSettingsConfigurable : Configurable {

    private var panel: DialogPanel? = null

    override fun getDisplayName(): String = "BWSL"

    override fun createComponent(): JComponent {
        val settings = BwslSettings.getInstance()
        return panel {
            row("Compiler path:") {
                textFieldWithBrowseButton(
                    FileChooserDescriptor(true, false, false, false, false, false)
                        .withTitle("Select BWSL Compiler")
                ).bindText(settings::compilerPath)
            }
        }.also { panel = it }
    }

    override fun isModified(): Boolean = panel?.isModified() ?: false
    override fun apply() { panel?.apply() }
    override fun reset() { panel?.reset() }
    override fun disposeUIResources() { panel = null }
}
