#!/bin/sh

ORIG=gswap2.3
LIST="gswap3.3 gswap4.3 gswap8.3 gswap2a.3 gswap4a.3 gswap8a.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_doy2md.3
LIST="ms_md2doy.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_genfactmult.3
LIST="ms_ratapprox.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_lookup.3
LIST="get_samplesize.3 get_encoding.3 get_blktdesc.3 get_blktlen.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_readmsr.3
LIST="ms_readtraces.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_strncpclean.3
LIST="ms_strncpopen.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=ms_time.3
LIST="ms_btime3hptime.3 ms_btime2isotimestr.3 ms_btime2seedtimestr.3 ms_hptime2btime.3 ms_hptime2isotimestr.3 ms_hptime2seedtimestr.3 ms_time2hptime.3 ms_seedtimestr2hptime.3 ms_timestr2hptime.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=msr_init.3
LIST="msr_free.3 msr_free_blktchain.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=msr_pack.3
LIST="msr_pack_header.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=msr_print.3
LIST="msr_srcname.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=msr_samprate.3
LIST="msr_nomsamprate.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=msr_starttime.3
LIST="msr_starttime_uc.3 msr_endtime.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_addmsr.3
LIST="mst_addspan.3 mst_addmsrtogroup.3 mst_addtracetogroup.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_findmatch.3
LIST="mst_findadjacent.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_groupsort.3
LIST="mst_heal.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_init.3
LIST="mst_free.3 mst_initgroup.3 mst_freegroup.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_pack.3
LIST="mst_packgroup.3"
for link in $LIST ; do
    ln -s $ORIG $link
done

ORIG=mst_printtracelist.3
LIST="mst_printgaplist.3 mst_srcname.3"
for link in $LIST ; do
    ln -s $ORIG $link
done
