#!/bin/sh

echo "Content-Type: application/json"
echo "Cache-Control: no-store"
echo

json_escape_stream() {
	awk '
		BEGIN { ORS = "" }
		{
			if (NR > 1) printf "\\n"
			gsub(/\\/, "\\\\")
			gsub(/"/, "\\\"")
			gsub(/\t/, "\\t")
			gsub(/\r/, "\\r")
			gsub(/[\001-\010\013\014\016-\037]/, "")
			printf "%s", $0
		}
	'
}

json_escape_file_stream() {
	if command -v python3 >/dev/null 2>&1; then
		python3 -c 'import json, sys; sys.stdout.write(json.dumps(sys.stdin.buffer.read().decode("utf-8", "replace"))[1:-1])'
	else
		json_escape_stream
	fi
}

json_escape() {
	printf '%s' "$1" | json_escape_stream
}

json_pair() {
	printf '"%s":"%s"' "$1" "$(json_escape "$2")"
}

query_value() {
	key=$1
	printf '%s' "$QUERY_STRING" | tr '&' '\n' | awk -F= -v k="$key" '$1 == k { print substr($0, length($1) + 2); exit }'
}

decode_value() {
	printf '%s' "$1" | sed \
		-e 's/+/ /g' \
		-e 's/%2[fF]/\//g' \
		-e 's/%2[eE]/./g' \
		-e 's/%20/ /g' \
		-e 's/%5[fF]/_/g' \
		-e 's/%2[dD]/-/g'
}

root_path() {
	case "$1" in
		jffs2) echo /mnt/jffs2 ;;
		media) echo /media ;;
		opt) echo /opt ;;
		logs) echo /var/log ;;
		web) echo /www ;;
		*) return 1 ;;
	esac
}

clean_rel() {
	rel=$1
	case "$rel" in
		""|.) echo "" ;;
		/*|*../*|../*|*"/.."|*..*)
			return 1
			;;
		*[\`\"\$\\\;\|\&\<\>]*)
			return 1
			;;
		*)
			printf '%s' "$rel" | sed 's#^\./##; s#//*#/#g; s#/$##'
			;;
	esac
}

file_type() {
	[ -d "$1" ] && {
		echo dir
		return
	}
	[ -L "$1" ] && {
		echo link
		return
	}
	[ -f "$1" ] && {
		echo file
		return
	}
	echo other
}

file_size() {
	[ -f "$1" ] || {
		echo 0
		return
	}
	wc -c < "$1" 2>/dev/null | tr -d ' '
}

list_dir() {
	root_name=$1
	rel=$2
	base=$(root_path "$root_name") || {
		printf '{"ok":false,"error":"bad_root"}\n'
		return
	}
	rel=$(clean_rel "$rel") || {
		printf '{"ok":false,"error":"bad_path"}\n'
		return
	}
	target=$base
	[ -n "$rel" ] && target=$base/$rel
	[ -d "$target" ] || {
		printf '{"ok":false,"error":"not_directory"}\n'
		return
	}

	printf '{"ok":true,'
	json_pair root "$root_name"; printf ','
	json_pair path "$rel"; printf ','
	json_pair base "$base"; printf ','
	printf '"entries":['
	first=1
	for item in "$target"/* "$target"/.[!.]* "$target"/..?*; do
		[ -e "$item" ] || continue
		name=$(basename "$item")
		if [ "$name" = "." ] || [ "$name" = ".." ]; then
			continue
		fi
		[ $first -eq 0 ] && printf ','
		printf '{'
		json_pair name "$name"; printf ','
		json_pair type "$(file_type "$item")"; printf ','
		json_pair size "$(file_size "$item")"
		printf '}'
		first=0
	done
	printf ']}\n'
}

read_file() {
	root_name=$1
	rel=$2
	base=$(root_path "$root_name") || {
		printf '{"ok":false,"error":"bad_root"}\n'
		return
	}
	rel=$(clean_rel "$rel") || {
		printf '{"ok":false,"error":"bad_path"}\n'
		return
	}
	target=$base/$rel
	[ -f "$target" ] || {
		printf '{"ok":false,"error":"not_file"}\n'
		return
	}
	printf '{"ok":true,'
	json_pair root "$root_name"; printf ','
	json_pair path "$rel"; printf ','
	json_pair size "$(file_size "$target")"; printf ','
	printf '"truncated":"%s",' "$([ "$(file_size "$target")" -gt 32768 ] && echo 1 || echo 0)"
	printf '"content":"'
	dd if="$target" bs=1024 count=32 2>/dev/null | json_escape_file_stream
	printf '"}\n'
}

action=$(decode_value "$(query_value action)")
root=$(decode_value "$(query_value root)")
path=$(decode_value "$(query_value path)")
[ -n "$root" ] || root=jffs2

case "$action" in
	read)
		read_file "$root" "$path"
		;;
	*)
		list_dir "$root" "$path"
		;;
esac
