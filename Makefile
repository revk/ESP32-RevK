all:	revk_settings idfmon

revk_settings: revk_settings.c
	gcc -O -o $@ $< -g -Wall --std=gnu99 -lpopt

idfmon: idfmon.c
	gcc -O -o $@ $< -g -Wall --std=gnu99
