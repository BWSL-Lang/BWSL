package com.bwsl.plugin.psi

import com.bwsl.plugin.BwslFileType
import com.bwsl.plugin.BwslLanguage
import com.intellij.extapi.psi.PsiFileBase
import com.intellij.openapi.fileTypes.FileType
import com.intellij.psi.FileViewProvider

class BwslFile(viewProvider: FileViewProvider) : PsiFileBase(viewProvider, BwslLanguage) {
    override fun getFileType(): FileType = BwslFileType.INSTANCE
    override fun toString(): String = "BWSL File"
}
