package com.bwsl.plugin

import com.intellij.lang.annotation.AnnotationHolder
import com.intellij.lang.annotation.Annotator
import com.intellij.lang.annotation.HighlightSeverity
import com.intellij.psi.PsiElement
import com.intellij.psi.impl.source.tree.LeafPsiElement

class BwslAnnotator : Annotator {
    override fun annotate(element: PsiElement, holder: AnnotationHolder) {
        if (element is LeafPsiElement
            && element.elementType == BwslTokenTypes.IDENTIFIER
            && element.text in BwslIntrinsics.NAMES
        ) {
            holder.newSilentAnnotation(HighlightSeverity.INFORMATION)
                .range(element.textRange)
                .textAttributes(BwslSyntaxHighlighter.INTRINSIC)
                .create()
        }
    }
}
