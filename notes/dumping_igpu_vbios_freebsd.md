# dumping amd (i)gpu vbios under FreeBSD

*if you are using a dedicated gpu, please note that
the output may be lacking the builtin gop driver*

- install the `acpica-tools` package
- get `vbios_vfct_file.c` from this repo
- run the following:
```sh
tmpdir=`mktemp -d`
cd $tmpdir

# specifying a full path here because of a name collision in $PATH
sudo /usr/local/bin/acpidump -b

cp ~/Downloads/vbios_vfct_file.c
cc vbios_vfct_file.c
./a.out vfct.dat
```

this should retrieve you a vbios file.
