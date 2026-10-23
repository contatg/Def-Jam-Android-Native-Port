# Def Jam FFNY Android ARM64 — build local

Este bring-up usa o runtime e o Vulkan do próprio Def Jam. O Android fornece
somente a janela SDL3, ciclo de vida e caminhos de armazenamento. Nenhum arquivo
do jogo entra no projeto ou no APK público.

## Dependências locais

O build nunca usa `git`. Na primeira execução, o script de preparação baixa
ZIPs de versão fixa e os guarda em `android/vendor/packages`:

- SDL 3.4.16 em `android/vendor/SDL`;
- Dear ImGui 1.92.9b em `android/vendor/imgui`;
- shaderc é compilado localmente a partir da cópia incluída no NDK.

Para uma máquina sem internet, baixe antes os ZIPs do SDL e ImGui em outro PC e rode:

```powershell
.\android\Preparar-Dependencias-Android.ps1 -SDLZip "C:\pacotes\SDL.zip" -ImGuiZip "C:\pacotes\imgui.zip"
```

Também são necessários no PC: JDK 17, Android SDK 34, um NDK instalado,
CMake 3.31.6, Python e Gradle já instalados ou armazenados no cache local.

## Dump

Use somente o dump USA verificado. A pasta passada em `-DataDir` deve conter:

```text
<DataDir>/extracted/default.xbe
<DataDir>/extracted/assets/
<DataDir>/extracted/audio/
...
```

O script recusa outro `default.xbe`. A análise e o código recompilado ficam
somente no computador e continuam ignorados pelo controle de versão.

## Compilar

No PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
cd C:\caminho\DefJamFFNY-recomp
.\android\Compilar-DefJam-Android.ps1 -DataDir "C:\Games\DefJamFFNY-data" -Variant Debug
```

Use `-Regenerate` depois de alterar o lifter, os símbolos ou o dump. Mudanças
apenas no host Android não precisam repetir a recompilação do XBE.

## Celular

O aplicativo cria:

```text
/storage/emulated/0/DefJamFFNY/game
```

Copie para essa pasta o dump extraído já verificado. Configurações, saves,
logs e cache ficam em `/storage/emulated/0/DefJamFFNY`.

Instale e colete o Logcat com:

```powershell
.\android\Instalar-e-Coletar-Logs.ps1 -Apk ".\android\app\build\outputs\apk\debug\app-debug.apk"
```

O primeiro marco é o APK carregar `libmain.so` e iniciar o Vulkan. Compilar o
APK não comprova menu ou gameplay; isso precisa do log e de teste no aparelho.
