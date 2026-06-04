set -e

antcc=$(realpath "../../antcc")

get() {
    url=$1
    out=$2
    file=${url##*/}
    case "$file" in
    *.tar.gz) dir=${file%.tar.gz}
             ext=tar.gz
             cmd="tar xf"
             ;;
    *.zip) dir=${file%.zip}
           ext=zip
           cmd=unzip
           ;;
    *) echo " ! ?$file" ; exit 1
        ;;
    esac
    if ! test -d "$out"; then
        archive=$out.$ext
        if ! test -f "$archive"; then
            echo "+ get $url"
            if ! curl -m10 -fL "$url" --output "$archive"; then
                echo " + ... download error"
                exit 0
            fi
        else
            echo "+ have $archive"
        fi
        echo "+ unpk to $out"
        $cmd "$archive"
    else
        echo "+ have $out/"
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
