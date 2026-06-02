plugins {
    id("java")
    id("org.jetbrains.kotlin.jvm") version "2.3.21"
    id("org.jetbrains.intellij.platform") version "2.16.0"
}

group = "com.bwsl.jetbrains_plugin"
version = "1.0.0"

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        intellijIdea("2026.1.2")
        bundledPlugin("com.intellij.java")
        testFramework(org.jetbrains.intellij.platform.gradle.TestFrameworkType.Platform)

    }
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "252"
        }

        changeNotes = """
      Initial version
    """.trimIndent()
    }
}

tasks {
    // Set the JVM compatibility versions
    withType<JavaCompile> {
        sourceCompatibility = "21"
        targetCompatibility = "21"
    }

    buildSearchableOptions {
        enabled = false
    }
}
