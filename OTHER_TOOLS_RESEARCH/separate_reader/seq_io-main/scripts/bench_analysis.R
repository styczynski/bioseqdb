library(ggplot2)
library(cowplot)

criterion_dir <- commandArgs(TRUE)[1]

sessionInfo()

#### read data ####

d <- lapply(list.dirs(criterion_dir, F, F), function(dir) {
  read.csv(file.path(criterion_dir, dir, 'new', 'raw.csv'))
})
d <- do.call(rbind, d)

s <- strsplit(as.character(d$group), ' ')
cfg <- as.data.frame(t(as.data.frame(lapply(s, '[', 1:5))))
names(cfg) <- c('format', 'reader', 'seqlen', 'config', 'data_len')
cfg$data_len <- as.numeric(as.character(cfg$data_len))
cfg$seqlen <- as.numeric(as.character(cfg$seqlen))
cfg$cap_test <- grepl('_cap', as.character(cfg$reader))

d <- cbind(d, cfg)

d$gb_per_s <- (as.numeric(d$data_len) / 1e9) / (d$sample_measured_value / 1e9) * d$iteration_count

d <- subset(d, !is.na(data_len))


#### setup output directory ####

outdir <- file.path(dirname(dirname(criterion_dir)), 'bench_results')
dir.create(outdir, F)


#### comparison of readers ####

reader_plot <- function(data, facets) {
  ggplot(data, aes(reader, gb_per_s, fill=reader)) +
    stat_summary(fun=mean, geom='bar', width=1, colour='#222222', linewidth=0.2) +
    stat_summary(fun.data=mean_se, geom = 'errorbar', width=0.2, alpha=0.5) +
    facet_grid(as.formula(paste(paste(facets, collapse= "+"), " ~ .")),
               space='free_y', scale='free_y') +
    scale_fill_grey(start=0.05, end=0.95) +
    coord_flip(expand=F, ylim=c(0, max(data$gb_per_s, na.rm=T))) +
    labs(x='reader', y='GB/s') +
    theme_bw() +
    theme(
      strip.text.y = element_text(angle=0),
      strip.background = element_rect(fill='white', colour='gray50'),
      axis.text.y = element_blank(),
      axis.ticks.y = element_blank(),
      axis.title = element_blank(),
      panel.grid.major.y = element_blank(),
      panel.grid.minor.y = element_blank(),
      panel.spacing = unit(-.5, 'pt')
    )
}

reader_cmp <- subset(d, !cap_test)

full <- lapply(split(reader_cmp, reader_cmp$format), function(data) {
    reader_plot(data, c('seqlen', 'config'))
})

reader_cmp_filter <- subset(reader_cmp, seqlen == 500 & !grepl('(records|seq|iter)', config, perl=T))
simple <- lapply(split(reader_cmp_filter, reader_cmp_filter$format), function(data) {
    reader_plot(data, 'config')
})

no_legend <- theme(legend.position='none')

png(file.path(outdir, 'reader_comparison.png'), width=1400, height=700, res=150)
plot_grid(NULL, NULL,
          full$fasta + no_legend, full$fastq,
          rel_widths=c(10, 13), rel_heights=c(1, 12),
          labels=c('FASTA', 'FASTQ'), label_size=12)
dev.off()

png(file.path(outdir, 'reader_comparison_simple.png'), width=1400, height=250, res=150)
plot_grid(NULL, NULL,
          simple$fasta + no_legend, simple$fastq,
          rel_widths=c(10, 13), rel_heights=c(2, 12),
          labels=c('FASTA', 'FASTQ'), label_size=12)
dev.off()


for (fmt in levels(reader_cmp$format)) {
  data <- subset(reader_cmp, format == fmt & !is.na(data_len))
  png(file.path(outdir, sprintf('bench_%s.png', fmt)), width=1400, height=30*length(unique(data$group))+150, res=200)
  print(reader_plot(data, c('seqlen', 'config')))
  dev.off()

  sub <- subset(data, seqlen == 500 & !grepl('(records|seq|iter)', config, perl=T))
  png(file.path(outdir, sprintf('bench_%s_simple.png', fmt)), width=1400, height=30*length(unique(sub$group))+150, res=200)
  print(reader_plot(sub, c('config')))
  dev.off()
}


#### test different buffer capacities ####

cap_cmp <- subset(d, cap_test)

png(file.path(outdir, 'bench_cap.png'), width=1400, height=1000, res=200)
cap_cmp$bufsize <- as.numeric(gsub('([0-9]+)ki', '\\1', cap_cmp$config))
ggplot(cap_cmp, aes(bufsize, gb_per_s, color=as.factor(seqlen), linetype=format)) +
    stat_summary(fun=mean, geom='point') +
    stat_summary(fun=mean, geom='line') +
    stat_summary(fun.data=mean_se, geom = 'errorbar', width=0.1, alpha=0.5) +
    expand_limits(y=0) +
    scale_x_continuous(trans='log1p', breaks=2^(0:25)) +
    labs(x='Buffer size (KiB)', y='GB/s', color='Sequence length', linetype='Format') +
    scale_color_brewer(palette='Set1') +
    theme_bw()
dev.off()
