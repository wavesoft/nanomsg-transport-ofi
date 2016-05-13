
.PHONY: clean 
SOURCES = ofiTransport.tex atex.tex 

pdf: $(SOURCES) clean 
#	pdflatex -interaction=batchmode cvmfstech > /dev/null
	pdflatex -interaction=batchmode ofiTransport	
	bibtex ofiTransport 
	makeindex ofiTransport.idx
	pdflatex -interaction=batchmode ofiTransport > /dev/null
	pdflatex -interaction=batchmode ofiTransport | grep -i 'overful|underful' || true
	thumbpdf ofiTransport
	pdflatex -interaction=batchmode ofiTransport > /dev/null
	pdfopt ofiTransport.pdf ofiTransport.pdf.opt
	rm -f ofiTransport.pdf
	mv ofiTransport.pdf.opt ofiTransport.pdf

clean: 
	rm -f ofiTransport.idx ofiTransport.log ofiTransport.toc cvmfs-blocks.tex 
	rm -f ofiTransport.ilg ofiTransport.ind ofiTransport.tpt q.log texput.log cernlogo.log ofiTransport.out
	rm -f ofiTransport.bbl ofiTransport.blg ofiTransport.lol
	rm -f content/protocol.bbl content/protocol.log content/protocol.blg
	find . -name "*.aux" -exec rm {} \;

