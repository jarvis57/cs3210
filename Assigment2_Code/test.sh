mkdir ../Gen-Data
for livePercent in 0 25 50 75 100
do
	for size in 5 3000
	do
		./genWorld $size $livePercent "../Gen-Data/random$size-$livePercent.txt"
	done
done

make
make -f makefile.lab

mkdir ../Result
for t in 1
do
	for w in 25 50 75 100
	do
		for p in 25 50 75 100
		do
			./SETL "../Gen-Data/random3000-$w.txt" 100 "../Gen-Data/random5-$p.txt" >test.ans
			mpirun -machinefile machinefile.lab -rankfile rankfile.lab -np 12 ./SETL_par "../Gen-Data/random3000-$w.txt" 100 "../Gen-Data/random5-$p.txt" >test.out
			diff test.out test.ans ">../Result/run$t-w$w-p$p.txt"
			echo "--------------------------------------------------------------"
			echo "Done attempt#$t: w $w, p$p"
			cat ">../Result/run$t-w$w-p$p.txt"
		done
	done
done
