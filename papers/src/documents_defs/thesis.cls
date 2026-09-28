% Klasa dokumentow do skladu prac magisterskich i~licencjackich
% na wydziale Matematyki, Mechaniki i~Informatyki UW.
%
% Copyright (c) 2001 by Wydzial Matematyki, Informatyki i Mechaniki.
%
% Zmiany 05.05.2006 by Seweryn Karlowicz
%
% Zmiany 03.01.2017 by Kuba Pochrybniak
%
% Zmiany 10.06.2020 by Paweł Goldstein
\usepackage{etoolbox}
\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{thesis}[2017/01/03 v0.7.0 Praca magisterska]


\def\@baseclass{report}
\def\@rodzajpracy{\if\dataeq{languages.0}{en-US} Master's\else magisterska\fi}
\def\@stopien{\if\dataeq{languages.0}{en-US} Master\else magister\fi}
\DeclareOption{licencjacka}{
	\def\@stopien{\if\dataeq{languages.0}{en-US} Bachelor\else licencjat\fi}
	\def\@rodzajpracy{\if\dataeq{languages.0}{en-US} Bachelor's\else licencjacka\fi}
}

\DeclareOption*{\PassOptionsToClass{\CurrentOption}{\@baseclass}}
\PassOptionsToClass{a4paper,twoside,openright,11pt}{\@baseclass}
\ProcessOptions

\LoadClass{\@baseclass}

\textwidth\paperwidth
\advance\textwidth -55mm
\oddsidemargin-1in
\advance\oddsidemargin 30mm
\evensidemargin-1in
\advance\evensidemargin 25mm
\topmargin -1in
\advance\topmargin 2cm
\setlength\textheight{48\baselineskip}
\addtolength\textheight{\topskip}
\marginparwidth15mm

\newcounter{liczbaAutorow}
\setcounter{liczbaAutorow}{0}
\def\autor#1#2{\def\@imienazwisko{#1}\def\@album{#2}\stepcounter{liczbaAutorow}}
\def\autori#1#2{\def\@imienazwiskoi{#1}\def\@albumi{#2}\stepcounter{liczbaAutorow}}
\def\autorii#1#2{\def\@imienazwiskoii{#1}\def\@albumii{#2}\stepcounter{liczbaAutorow}}
\def\autoriii#1#2{\def\@imienazwiskoiii{#1}\def\@albumiii{#2}\stepcounter{liczbaAutorow}}
\def\autoriv#1#2{\def\@imienazwiskoiv{#1}\def\@albumiv{#2}\stepcounter{liczbaAutorow}}
\def\autorv#1#2{\def\@imienazwiskov{#1}\def\@albumv{#2}\stepcounter{liczbaAutorow}}

% \def\abstract#1{\def\@cabstract{#1}}
% \def\intro#1{\def\@cintro{#1}}

\autor{}{}
\autori{}{}
\autorii{}{}
\autoriii{}{}
\autoriv{}{}
\autorv{}{}
\setcounter{liczbaAutorow}{0}

\newcommand{\autorZAlbumem}[2]{\begin{minipage}{0.45\textwidth}\centering{\Large\bfseries #1}\\[2mm]\if\dataeq{languages.0}{en-US} Student no.\else Nr albumu:\fi\ #2\end{minipage}\ }

% Optional dedication on the existing page immediately after the title.
\newcommand{\ThesisPostTitleContent}{%
  \if\dataeq{acknowledgments.path}{??}%
    \vfill\mbox{}%
  \else
    \datapartial{acknowledgments}%
  \fi
}

\if\dataeq{languages.0}{en-US}
	\renewcommand\maketitle{%
	  \begin{titlepage}%
		\let\footnotesize\small
		\let\footnoterule\relax
		\let \footnote \thanks
		\begin{center}%
		  {\LARGE\textbf{ University of Warsaw}\\
			Faculty of Mathematics, Informatics and Mechanics\par}
		  \vspace{1cm plus .5fill}
		{
			\setlength{\parindent}{0em}
			\setlength{\baselineskip}{60pt}
			\centering

			\ifx\@imienazwisko\empty\else\autorZAlbumem{\@imienazwisko}{\@album}\fi
			\ifx\@imienazwiskoi\empty\else\autorZAlbumem{\@imienazwiskoi}{\@albumi}\fi
			\ifx\@imienazwiskoii\empty\else\autorZAlbumem{\@imienazwiskoii}{\@albumii}\fi
			\ifx\@imienazwiskoiii\empty\else\autorZAlbumem{\@imienazwiskoiii}{\@albumiii}\fi
			\ifx\@imienazwiskoiv\empty\else\autorZAlbumem{\@imienazwiskoiv}{\@albumiv}\fi
			\ifx\@imienazwiskov\empty\else\autorZAlbumem{\@imienazwiskov}{\@albumv}\fi

		}

		  \vspace{8mm plus .5fill}
		  {\Huge\textbf{\@title}\par}
		  \vspace{8mm plus .1fill}
		  {\large\bf \@rodzajpracy\ thesis\\[3pt]
			in \MakeUppercase{\@kierunek} \\
			\@zakres \par}
		  \vspace{2cm plus 1.5fill}
		  \ifx\@opiekun\empty\else
		    \begin{flushright}\large
			  \begin{tabular}{l}
			    Supervisor:\\
			    \bfseries \@opiekun
			  \end{tabular}
		    \end{flushright}
		  \fi
		  \vspace{1cm plus 1fill}
		  {\large Warsaw, \@date\par}
		\end{center}
		\@thanks
		\end{titlepage}%
		\begin{titlepage}
	 		\c@page=2
	 		\ThesisPostTitleContent
	 	\end{titlepage}


	  \begin{titlepage}
	  \c@page=2
	  \end{titlepage}
	  \setcounter{footnote}{0}%
	  \global\let\thanks\relax
	  \global\let\maketitle\relax
	  \global\let\@thanks\@empty
	  \global\let\@author\@empty
	  \global\let\@date\@empty
	  \global\let\@title\@empty
	  \global\let\title\relax
	  \global\let\author\relax
	  \global\let\date\relax
	  \global\let\and\relax
	}
	\def\nralbumu#1{\gdef\@nralbumu{#1}}
	\def\@nralbumu{???\ClassError{pracamgr}{Brak numeru albumu}\@ehc}
	\def\kierunek#1{\gdef\@kierunek{#1}}
	\def\@kierunek{???\ClassError{pracamgr}{Nie podano kierunku studiow}\@ehc}
	\def\zakres#1{\gdef\@zakres{w zakresie \MakeUppercase{#1}}}
	\def\@zakres{}
	\def\opiekun#1{\gdef\@opiekun{#1}}
	\def\@opiekun{???\ClassError{pracamgr}{Brak danych opiekuna pracy}\@ehc}
	\def\keywords#1{\gdef\@keywords{#1}}
	\def\@keywords{???\ClassError{pracamgr}{Brak slow kluczowych}\@ehc}
	\def\dziedzina#1{\gdef\@dziedzina{#1}}
	\def\@dziedzina{???\ClassError{pracamgr}{Brak dziedziny Socrates-Erasmus}\@ehc}
	\def\klasyfikacja#1{\gdef\@klasyfikacja{#1}}
	\def\@klasyfikacja{???\ClassError{pracamgr}{Brak klasyfikacji tematycznej}\@ehc}
	\def\tytulang#1{\gdef\@tytulang{#1}}
	\def\@tytulang{???\ClassError{pracamgr}{Brak tytulu po angielsku}\@ehc}
	\def\titleen#1{\title{#1}}
	\def\titlepl#1{\gdef\@titlepl{#1}}
	\def\@titlepl{???\ClassError{pracamgr}{Brak tytulu po polsku}\@ehc}


	\renewenvironment{abstract}{%
	  \titlepage
	  \null\nobreak\vfil
	  \@beginparpenalty\@lowpenalty
	  \begin{center}%
		\bfseries\large \abstractname
		\@endparpenalty\@M
	  \end{center}}%
	{\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Keywords
		\@endparpenalty\@M
	  \end{center}
	  \@keywords\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Thesis domain (Socrates-Erasmus subject area codes)
		\@endparpenalty\@M
	  \end{center}
	   \@dziedzina\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Subject classification
		\@endparpenalty\@M
	  \end{center}
	  \@klasyfikacja\par
	  \vspace*{26pt}%
	  \begin{center}%
		\begingroup
		\bfseries\large Tytuł pracy w języku polskim
		\@endparpenalty\@M
		\endgroup

		\medskip
		\@titlepl\par
	  \end{center}
	  \vspace*{26pt}%
	  \nobreak\vfil\null\endtitlepage\cleardoublepage}


\def\cleardoublepage{\clearpage\if@twoside \ifodd\c@page\else
    \hbox{}\thispagestyle{empty}\newpage\if@twocolumn\hbox{}\newpage\fi\fi\fi}

\renewcommand*\@seccntformat[1]{\csname the#1\endcsname.\enspace}
\def\numberline#1{\hb@xt@\@tempdima{#1.\hfil}}
\renewcommand*\l@chapter[2]{%
  \ifnum \c@tocdepth >\m@ne
    \addpenalty{-\@highpenalty}%
    \vskip 1.0em \@plus\p@
    \setlength\@tempdima{1.5em}%
    \begingroup
      \parindent \z@ \rightskip \@pnumwidth
      \parfillskip -\@pnumwidth
      \leavevmode \bfseries
      \advance\leftskip\@tempdima
      \hskip -\leftskip
      #1\nobreak\mdseries
      \leaders\hbox{$\m@th
        \mkern \@dotsep mu\hbox{.}\mkern \@dotsep
        mu$}\hfill
      \nobreak\hb@xt@\@pnumwidth{\hss #2}\par
      \penalty\@highpenalty
    \endgroup
  \fi}
\else

	\renewcommand\maketitle{%
	  \begin{titlepage}%
		\let\footnotesize\small
		\let\footnoterule\relax
		\let \footnote \thanks
		\begin{center}%
		  {\LARGE\textbf{Uniwersytet Warszawski}\\
			Wydzia\l{} Matematyki, Informatyki i Mechaniki\par}
		  \vspace{1cm plus .5fill}
		{
			\setlength{\parindent}{0em}
			\centering

				\ifx\@imienazwisko\empty\else\autorZAlbumem{\@imienazwisko}{\@album}\fi
				\ifx\@imienazwiskoi\empty\else\autorZAlbumem{\@imienazwiskoi}{\@albumi}\fi
				\ifx\@imienazwiskoii\empty\else\autorZAlbumem{\@imienazwiskoii}{\@albumii}\fi
				\ifx\@imienazwiskoiii\empty\else\autorZAlbumem{\@imienazwiskoiii}{\@albumiii}\fi
				\ifx\@imienazwiskoiv\empty\else\autorZAlbumem{\@imienazwiskoiv}{\@albumiv}\fi
				\ifx\@imienazwiskov\empty\else\autorZAlbumem{\@imienazwiskov}{\@albumv}\fi

		}

		  \vspace{8mm plus .5fill}
		  {\Huge\textbf{\@title}\par}
		  \vspace{8mm plus .1fill}
		  {\large\bf Praca \@rodzajpracy\\[3pt]
			na kierunku \MakeUppercase{\@kierunek} \\
			\@zakres \par}
		  \vspace{2cm plus 1.5fill}
		  \ifx\@opiekun\empty\else
		    \begin{flushright}\large
			  \begin{tabular}{l}
			    Praca wykonana pod kierunkiem\\
			    \bfseries \@opiekun
			  \end{tabular}
		    \end{flushright}
		  \fi
		  \vspace{1cm plus 1fill}
		  {\large Warszawa, \@date\par}
		\end{center}
		\@thanks
	  \end{titlepage}%
	   \begin{titlepage}
	  \c@page=2
	\ThesisPostTitleContent
	  \end{titlepage}

	  \setcounter{footnote}{0}%
	  \global\let\thanks\relax
	  \global\let\maketitle\relax
	  \global\let\@thanks\@empty
	  \global\let\@author\@empty
	  \global\let\@date\@empty
	  \global\let\@title\@empty
	  \global\let\title\relax
	  \global\let\author\relax
	  \global\let\date\relax
	  \global\let\and\relax
	}
	\def\nralbumu#1{\gdef\@nralbumu{#1}}
	\def\@nralbumu{???\ClassError{pracamgr}{Brak numeru albumu}\@ehc}
	\def\kierunek#1{\gdef\@kierunek{#1}}
	\def\@kierunek{???\ClassError{pracamgr}{Nie podano kierunku studiow}\@ehc}
	%----zakres nie konieczny-----
	\def\zakres#1{\gdef\@zakres{w zakresie \MakeUppercase{#1}}}
	\def\@zakres{}
	\def\opiekun#1{\gdef\@opiekun{#1}}
	\def\@opiekun{???\ClassError{pracamgr}{Brak danych opiekuna pracy}\@ehc}
	\def\keywords#1{\gdef\@keywords{#1}}
	\def\@keywords{???\ClassError{pracamgr}{Brak slow kluczowych}\@ehc}
	\def\dziedzina#1{\gdef\@dziedzina{#1}}
	\def\@dziedzina{???\ClassError{pracamgr}{Brak dziedziny Socrates-Erasmus}\@ehc}
	\def\klasyfikacja#1{\gdef\@klasyfikacja{#1}}
	\def\@klasyfikacja{???\ClassError{pracamgr}{Brak klasyfikacji tematycznej}\@ehc}
	%-------------------nowe------------
	\def\titleen#1{\gdef\@tytulang{#1}}
	\def\titlepl#1{\title{#1}}
	\def\@tytulang{???\ClassError{pracamgr}{Brak tytulu po angielsku}\@ehc}


	\renewenvironment{abstract}{%
	  \titlepage
	  \null\nobreak\vfil
	  \@beginparpenalty\@lowpenalty
	  \begin{center}%
		\bfseries\large \abstractname
		\@endparpenalty\@M
	  \end{center}}%
	{\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Słowa kluczowe
		\@endparpenalty\@M
	  \end{center}
	  \@keywords\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Dziedzina pracy (kody wg programu Socrates-Erasmus)
		\@endparpenalty\@M
	  \end{center}
	   \@dziedzina\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Klasyfikacja tematyczna
		\@endparpenalty\@M
	  \end{center}
	  \@klasyfikacja\par
	  \vspace*{26pt}%
	  \begin{center}%
		\bfseries\large Tytuł pracy w języku angielskim
		\@endparpenalty\@M
	  \end{center}
	  \@tytulang\par
	  \nobreak\vfil\null\endtitlepage\cleardoublepage}

	\def\cleardoublepage{\clearpage\if@twoside \ifodd\c@page\else
		\hbox{}\thispagestyle{empty}\newpage\if@twocolumn\hbox{}\newpage\fi\fi\fi}

	\renewcommand*\@seccntformat[1]{\csname the#1\endcsname.\enspace}
	\def\numberline#1{\hb@xt@\@tempdima{#1.\hfil}}
	\renewcommand*\l@chapter[2]{%
	  \ifnum \c@tocdepth >\m@ne
		\addpenalty{-\@highpenalty}%
		\vskip 1.0em \@plus\p@
		\setlength\@tempdima{1.5em}%
		\begingroup
		  \parindent \z@ \rightskip \@pnumwidth
		  \parfillskip -\@pnumwidth
		  \leavevmode \bfseries
		  \advance\leftskip\@tempdima
		  \hskip -\leftskip
		  #1\nobreak\mdseries
		  \leaders\hbox{$\m@th
			\mkern \@dotsep mu\hbox{.}\mkern \@dotsep
			mu$}\hfill
		  \nobreak\hb@xt@\@pnumwidth{\hss #2}\par
		  \penalty\@highpenalty
		\endgroup
	  \fi}

\fi

% \def\useintro{
% 	\begin{abstractblock}
% 		\@cabstract
% 	\end{abstractblock}
% 	\tableofcontents
% 	\chapter*{Introduction}
% 		\addcontentsline{toc}{chapter}{Introduction}
% 		\@cintro
% 		\begin{itemize}
% 			\chapterbrief{r:biology}{introduces the reader to the basics of bioinformatics, through sequence alignment and NGS. Next, the usual format workflow is presented.}
% 			\chapterbrief{r:postgres}{explores the extensibility of PostgreSQL and additional features that come in handy when processing big data, for example, TOAST format.}
% 		\end{itemize}
% }

%\newcommand\chapter[2]{\basechapter{#1}\label{#2}}
% \newenvironment{intro}{%
% 	\tableofcontents
% 	\chapter*{Introduction}
% 	\addcontentsline{toc}{chapter}{Introduction}
% 	\begin{itemize}
% 	\chapterbrief{r:biology}{introduces the reader to the basics of bioinformatics, through sequence alignment and NGS. Next, the usual format workflow is presented.}
% }{%
% 	\end{itemize}
% }
\def\defchapter#1#2{
	\chapter{#1}\label{#2}
}


\endinput
