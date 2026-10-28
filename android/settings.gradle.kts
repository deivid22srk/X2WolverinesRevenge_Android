// Configurações do projeto Gradle do port Android.
//
// Só existe UM módulo (`:app`) de propósito: o APK é a única saída do
// projeto. O runtime e o gen/ (código lifted) entram como fontes do CMake,
// não como módulos Gradle — assim o mesmo código serve Android e desktop.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "x2wr"
include(":app")
