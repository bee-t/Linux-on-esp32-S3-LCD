# If we are on the serial console and not already in epd-shell, launch it
if [ -c /dev/epd ] && [ -n "$PS1" ] && [ -z "$EPD_SHELL_ACTIVE" ]; then
    export EPD_SHELL_ACTIVE=1
    exec epd-shell
fi

