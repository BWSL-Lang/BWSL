package com.bwsl.plugin

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

class BwslFileType private constructor() : LanguageFileType(BwslLanguage) {
    companion object {
        @JvmField
        val INSTANCE = BwslFileType()
    }

    override fun getName(): String = "BWSL"
    override fun getDescription(): String = "BWSL shader file"
    override fun getDefaultExtension(): String = "bwsl"
    override fun getIcon(): Icon? = null
}
