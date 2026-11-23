# The plugin's library inside a MillSaph.ofx.bundle, for this system (sourced by the scripts
# that run build/test_host: . tools/plugin_path.sh).
case "$(uname)" in
Darwin) plugin_lib=Contents/MacOS/MillSaph.ofx ;;
*)      plugin_lib=Contents/Linux-x86-64/MillSaph.ofx ;;
esac
