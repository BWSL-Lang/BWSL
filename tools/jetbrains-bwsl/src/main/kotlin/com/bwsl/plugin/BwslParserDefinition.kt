package com.bwsl.plugin

import com.bwsl.plugin.psi.BwslFile
import com.intellij.extapi.psi.ASTWrapperPsiElement
import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet

class BwslParserDefinition : ParserDefinition {

    companion object {
        @JvmField val FILE        = IFileElementType(BwslLanguage)
        @JvmField val COMMENTS    = TokenSet.create(BwslTokenTypes.LINE_COMMENT, BwslTokenTypes.BLOCK_COMMENT)
        @JvmField val STRING_LITS = TokenSet.create(BwslTokenTypes.STRING_LIT)
    }

    override fun createLexer(project: Project?): Lexer = BwslLexerAdapter()

    override fun createParser(project: Project?): PsiParser = PsiParser { root, builder ->
        val marker = builder.mark()
        while (!builder.eof()) builder.advanceLexer()
        marker.done(root)
        builder.treeBuilt
    }

    override fun getFileNodeType(): IFileElementType                 = FILE
    override fun getWhitespaceTokens(): TokenSet                     = TokenSet.WHITE_SPACE
    override fun getCommentTokens(): TokenSet                        = COMMENTS
    override fun getStringLiteralElements(): TokenSet                = STRING_LITS
    override fun createElement(node: ASTNode): PsiElement           = ASTWrapperPsiElement(node)
    override fun createFile(viewProvider: FileViewProvider): PsiFile = BwslFile(viewProvider)
}
