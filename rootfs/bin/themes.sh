# themes.sh - show off every theme for a moment, then restore yours
saved=$(theme current)
for t in mirage mirage-light synthwave classic commander matrix amber nord dracula gruvbox paper mono ocean sunset
    theme $t
    clear
    banner
    echo -m "  {accent}theme $t{/}"
    echo
    fetch
    sleep 2
end
theme $saved
clear
