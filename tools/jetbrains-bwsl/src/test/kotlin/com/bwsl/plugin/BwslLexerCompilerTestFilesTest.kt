package com.bwsl.plugin

import com.intellij.psi.TokenType
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class BwslLexerCompilerTestFilesTest {

    companion object {
        private val COMPILER_TEST_SUBDIRS = listOf(
            "equivalence",
            "from_engine",
            "performance",
            "prod_shaders",
            "unsorted",
        )
    }

    @Test
    fun noBADCharacterTokensInCompilerTestFiles() {
        val testsRoot = File(System.getProperty("bwsl.test.compiler_test_dir", "../../tests"))

        val failures = mutableListOf<String>()
        var fileCount = 0

        for (subdir in COMPILER_TEST_SUBDIRS) {
            val dir = File(testsRoot, subdir)
            if (!dir.isDirectory) continue
            for (file in dir.listFiles { f -> f.extension == "bwsl" } ?: emptyArray()) {
                fileCount++
                val badTokens = lexForBadCharacters(file.readText())
                if (badTokens.isNotEmpty()) {
                    val details = badTokens.joinToString(", ") { (offset, ch) ->
                        "'$ch' (U+${ch.code.toString(16).uppercase().padStart(4, '0')}) at offset $offset"
                    }
                    failures += "$subdir/${file.name}: $details"
                }
            }
        }

        assertTrue(
            "Lexer produced BAD_CHARACTER tokens in $fileCount compiler test file(s) checked.\n" +
                failures.joinToString("\n"),
            failures.isEmpty()
        )
    }

    private fun lexForBadCharacters(content: String): List<Pair<Int, Char>> {
        val lexer = BwslLexerAdapter()
        lexer.start(content, 0, content.length, 0)
        val result = mutableListOf<Pair<Int, Char>>()
        while (lexer.tokenType != null) {
            if (lexer.tokenType == TokenType.BAD_CHARACTER) {
                val offset = lexer.tokenStart
                result += Pair(offset, content[offset])
            }
            lexer.advance()
        }
        return result
    }
}
