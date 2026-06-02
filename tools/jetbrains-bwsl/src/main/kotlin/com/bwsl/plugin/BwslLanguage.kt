package com.bwsl.plugin

import com.intellij.lang.Language

object BwslLanguage : Language("BWSL") {
    private fun readResolve(): Any = BwslLanguage
}
