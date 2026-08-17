rm -rf log
mkdir log

echo "start decompress ..."

cp -rf 1.log log/
##loop
i=2
n=2
while [ $i -lt 11 ];do
echo $i
tar -xf $i.tar.gz
cp -rf mnt/data/imi/imi_log/1.log log/$i.log
rm -rf mnt
i=$(($n+1))
n=$i
done

echo "end decompress !"
