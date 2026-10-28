// Build script raiz.
//
// Os plugins ficam declarados aqui com `apply false` para fixar UMA versão
// para todo o repositório (o módulo :app apenas aplica). Isso evita que o
// CI e uma máquina local resolvam AGP/Kotlin diferentes por acidente.
plugins {
    id("com.android.application") version "8.4.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
