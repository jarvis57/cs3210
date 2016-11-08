for livePercent in 0 10 20 30 40 50 60 70 80 90 100
do
	for size in 3 5 1000 3000
	do
		./genWorld $size $livePercent "../Gen-Data/random$size-$livePercent.txt"
	done
done
