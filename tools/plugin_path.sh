# This system's build folder ($build_dir: build/mac or build/lin; build/win is build.bat's),
# where a build puts the OFX bundle ($bundle_path: under $build_dir/, or $build_dir/sanitize/
# for tools/sanitize.sh) and the plugin's library inside the bundle ($plugin_lib). Sourced
# from the repository's root by the scripts that build or run the plugin: . tools/plugin_path.sh
case "$(uname)" in
Darwin) build_dir=build/mac plugin_lib=Contents/MacOS/MillSaph.ofx ;;
*)      build_dir=build/lin plugin_lib=Contents/Linux-x86-64/MillSaph.ofx ;;
esac
bundle_path=release/ofx/MillSaph.ofx.bundle
