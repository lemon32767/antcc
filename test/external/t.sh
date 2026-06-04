set -e

antcc=$(realpath "../../antcc")

get() {
    url=$1
    file=${url##*/}
    if ! test -f "$file"; then
        echo "+ get $url"
        curl -fL "$url" --output "$file"
    else
        echo "+ have $file"
    fi
    if ! test -d "$2"; then
        case "$file" in
            *.tar.*) dir=${file%.tar.*}
                     cmd="tar xf"
                     ;;
            *.zip) dir=${file%.zip}
                   cmd=unzip
                   ;;
            *) echo " ! ?$file" ; exit 1
                ;;
        esac
        echo "+ unpk to $2"
        $cmd "$file"
        if test -n "$3"; then
            dir=$3
        fi
        mv "$dir" "$2"
    else
        echo "+ have $2/"
    fi
}

gitget() {
    url=$1
    dir=${url##*/}
    if ! test -d "$dir"; then
        echo "+ clone $url -> $dir"
        git clone --depth 1 --revision $2 "$url" "$dir"
        (cd "$dir" && git checkout $3)
    else
        echo "+ have $dir/"
    fi
}

patch() {
    if ! test -f '.patched'; then
        echo "+ patching"
        echo "  + $@"
        "$@"
        touch .patched
    fi
}

#  vim:set ts=4 sw=4 expandtab: 
