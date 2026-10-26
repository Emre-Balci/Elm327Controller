// The app's native code is the CMake project at the root of the repository: its elm327gui target is
// the shared library of the native activity on Android (see gui/CMakeLists.txt). The version comes
// from there, too.
plugins {
    id("com.android.application")
}

val projectVersion: String = Regex("""VERSION (\d+)\.(\d+)\.(\d+)""")
    .find(file("../../CMakeLists.txt").readText())!!
    .destructured.let { (major, minor, patch) -> "$major.$minor.$patch" }

android {
    namespace = "io.github.emrebalci.elm327controller"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "io.github.emrebalci.elm327controller"
        // Common needs Android 9 (API level 28) and a 64-bit ABI.
        minSdk = 28
        targetSdk = 36
        versionName = projectVersion
        versionCode = projectVersion.split(".").fold(0) { code, part -> code * 100 + part.toInt() }
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DELM327_BUILD_GUI=ON",
                    "-DELM327_BUILD_CLI=OFF",
                    "-DELM327_BUILD_SIMULATOR=OFF",
                    "-DELM327_BUILD_TESTS=OFF",
                )
                targets += "elm327gui"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
