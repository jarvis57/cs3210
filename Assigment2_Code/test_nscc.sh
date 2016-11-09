mkdir ../Gen-Data
make

for livePercent in 0 5
do
	./genWorld 3000 $livePercent "../Gen-Data/random3000-$livePercent.txt"
done
for livePercent in 0 5
do
	./genWorld 5 $livePercent "../Gen-Data/random5-$livePercent.txt"
done

mkdir ../Result
make -f makefile.nscc

for t in 1
do
	for w in 0 5
	do
		for p in 0 5
		do
			# ./SETL "../Gen-Data/random3000-$w.txt" 100 "../Gen-Data/random5-$p.txt" >test.ans
			mpirun -np 48 ./SETL_par "../Gen-Data/random3000-$w.txt" 10 "../Gen-Data/random5-$p.txt" >"test$w-$p.out"
			# diff test.out test.ans >"../Result/nscc-w$w-p$p.txt"
			echo "--------------------------------------------------------------"
			echo "Done attempt#$t: w$w, p$p"
			# cat "../Result/nscc$t-w$w-p$p.txt"
		done
	done
done
