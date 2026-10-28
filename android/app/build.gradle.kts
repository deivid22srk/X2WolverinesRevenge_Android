// Módulo :app — o APK (arm64-v8a) do port.
//
// Escopo deste arquivo: produzir o APK com a biblioteca nativa `libx2wr.so`.
// Todo o código do port (runtime + shims + código lifted) é compilado pelo
// CMake; o Gradle só empacota. Motivo: o mesmo CMake serve o build Linux de
// depuração em desktop/ — uma única lista de fontes para os dois alvos.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// ---- Caminho do código lifted (gen/) -------------------------------------
// `x2wr.genDir` permite ao CI apontar para onde o gen/ foi descompactado.
// O default resolve para <repo>/gen, que existe apenas em máquinas privadas
// e no CI "full" — no CI público ele NÃO existe, e o CMake cai no build
// "app-only" (ver cpp/CMakeLists.txt).
//
// CUIDADO COM A BASE: `file()` resolve relativo ao diretório do MÓDULO
// (android/app/), não à raiz de android/. Por isso "../../gen" (e não
// "../gen"): android/app -> android -> repo -> gen.
val x2wrGenDir: String =
    (project.findProperty("x2wr.genDir") as String?) ?: "../../gen"

// O runtime fica ao lado do gen/, na raiz do repositório.
val x2wrRuntimeDir: String =
    (project.findProperty("x2wr.runtimeDir") as String?) ?: "../../runtime"

// Converte para caminho absoluto: o CMake resolveria caminhos relativos
// contra o diretório do CMakeLists (android/app/src/main/cpp), que é CINCO
// níveis abaixo da raiz. Passar absoluto elimina a ambiguidade.
val x2wrGenDirAbs: String = file(x2wrGenDir).absolutePath
val x2wrRuntimeDirAbs: String = file(x2wrRuntimeDir).absolutePath

// Quantas partições de compilação o código lifted usa (LB_GEN_JOBS no CI).
// Repassado ao CMake apenas para diagnóstico/limites futuros de paralelismo.
val x2wrGenJobs: String =
    (project.findProperty("x2wr.genJobs") as String?) ?: "4"

android {
    namespace = "br.com.x2wr"
    compileSdk = 34

    // NDK pinável pelo CI (`-Px2wr.ndkVersion=27.0.12077973`). Se a
    // propriedade não existir, o AGP usa o NDK default dele — em máquina
    // local isso é o comportamento desejado (não força download de versão).
    project.findProperty("x2wr.ndkVersion")?.toString()?.let { ndkVersion = it }

    defaultConfig {
        applicationId = "br.com.x2wr"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        // SOMENTE arm64-v8a. O port é uma tradução estática de x86 para ARM64:
        // não há caminho de 32 bits e o código lifted não compilaria nele.
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                // Nada de flags de STL: runtime + código lifted são C puro.
                // O CMakeLists define X2WR_HAVE_GEN quando encontra gen/*.c.
                arguments += listOf(
                    "-DX2WR_GEN_DIR=$x2wrGenDirAbs",
                    "-DX2WR_RUNTIME_DIR=$x2wrRuntimeDirAbs",
                    "-DX2WR_GEN_JOBS=$x2wrGenJobs",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            // 3.22.1 é a versão instalada pelo CI (`cmake;3.22.1`). Fixar
            // aqui evita que o AGP escolha outro CMake do SDK local.
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            // O build de release é assinado com a CHAVE DE DEPURAÇÃO de
            // propósito: é um build de teste privado, distribuído apenas no
            // repositório privado do gen/. Isso torna o APK instalável lado
            // a lado com o de debug e dispensa o keystore de release (que
            // não existe no CI público).
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    sourceSets["main"].assets.srcDirs("src/main/assets")

    // View binding é usado no launcher (activity_main.xml). Não usei
    // findViewById para não repetir casts e não quebrar ao renomear views.
    buildFeatures {
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        // useLegacyPackaging = true: as .so saem do APK comprimidas e são
        // extraídas para o diretório de dados na instalação. Isso permite
        // carregar `libx2wr.so` por caminho absoluto (dlopen/System.load)
        // também em Android 6..8, sem depender de mmap não-alinhado.
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.activity:activity-ktx:1.9.0")
}
