% Style fuer TI-Diplom-, -Bachelor- und -Masterarbeiten
% Version "1.5" von 08/2014, MP -> Created cls file
% Kommentare und Anregungen an tkoehler @ ti...
% tiarbeit.sty:
% Version "1.1" von 02/2010, TK -> Hinweis auf pictexwd
% Version "1.0" von 07/2009, TK -> Bachelor- und Masterarbeiten, caption(2) raus
% tidipl.sty:
% Version "5" von 04/2008, (MK 12/2007) -> caption statt caption2 fuer subfig
% Version "4" von 09/2006, TK -> ngerman,wide,tall,abschliess-Erkl. no header
% Version "3" von 07/2006, TK von A.Schatz/S.Fischer : Z.99,100 -> 101ff
% Version "2" von 02/2004, TK
% Urspruenglich: imvolkme @ techfak...
\NeedsTeXFormat{LaTeX2e}[1995/12/01]
%\typeout{\space by Christiam Kiesewetter ...modified}
%\typeout{\space by M. Volkmer and T.Koehler :-) 1998/09/29}
%\typeout{*** !!! ***}
%\typeout{\space Mit thispagestyle{empty} entfallen Header und Footer}
%\typeout{\space und mit thispagestyle{fancyplain} entfaellt nur der Header.}
%\typeout{\space Benoetigt documentclass[__pt,a4paper]{report/book} !}
%\typeout{*** !!! ***}
\ProvidesClass{tiarbeit} %[1998/12/01 v0.0x
			 [2014/08/08 v1.5   TI-Arbeit-Class]
\LoadClassWithOptions{report}
%\DeclareOption{ngerman}{\def\@ngermantrue}  % default, not used
%\DeclareOption{english}{\def\@englishtrue} - not yet !
\ProcessOptions\relax

\RequirePackage[l2tabu, orthodox]{nag}

\RequirePackage{etex}
\RequirePackage[utf8]{inputenc}
\RequirePackage[T1]{fontenc}
\RequirePackage{lmodern}
\RequirePackage[final,babel]{microtype}

%\ifx\@englishtrue\undefined
%\RequirePackage[main=ngerman,british]{babel}
\newcommand*{\british}[1]{\foreignlanguage{british}{#1}}
%\else
\RequirePackage[main=english,ngerman]{babel}
%\fi

\RequirePackage{fancyhdr}
\RequirePackage{titlesec}
\RequirePackage[titles]{tocloft}
\RequirePackage{graphicx}
\RequirePackage[final]{listings}
\AtBeginDocument{\DeclareCaptionSubType{lstlisting}} % Makes subcaptions avaiable here. Must be placed after lstlsitings but before subcaption. See http://tex.stackexchange.com/a/63967
\RequirePackage[font={sf,small}, singlelinecheck=true, labelfont=bf]{caption}
\RequirePackage[round,colon]{natbib}
\RequirePackage{nomencl,makeidx} % tkoehler, 6.2.4
\RequirePackage{booktabs}
\RequirePackage{chngcntr} % to have continous footnotes
\RequirePackage[bottom]{footmisc} % to have the foot notes at the very bottom
\RequirePackage{xcolor}
\RequirePackage{siunitx}

% Setup fonts
%\renewcommand{\sfdefault}{phv} % Helvetica but slightly larger than other fonts
\sisetup{detect-all, binary-units, exponent-product=\cdot, range-phrase={ bis }, group-minimum-digits=4}

\renewcommand{\@makefntext}[1]{%
    \setlength{\parindent}{0pt}
    \begin{list}{}{%
        \setlength{\topsep}{0pt}
        \setlength{\partopsep}{0pt}
        \setlength{\labelwidth}{0.8em} % Space from number to border
        \setlength{\labelsep}{0.3em} % Space from number to text
        \setlength{\leftmargin}{\labelwidth}
        \addtolength{\leftmargin}{\labelsep}
        \footnotesize}
        \item[{\makebox[\labelwidth][r]{\@makefnmark}}]#1%
    \end{list}
}

\newcommand\cstfootnoterule{%
    \kern-3\p@
    \rule{.4\columnwidth}{0.4pt}
    \kern2.6\p@}

% Style of listof...
\renewcommand{\cftchapfont}{\sffamily\bfseries}
\renewcommand{\cftchappagefont}{\cftchapfont}
\renewcommand{\cftsecfont}{\sffamily}
\renewcommand{\cftsecpagefont}{\cftsecfont}
\renewcommand{\cftsubsecfont}{\sffamily}
\renewcommand{\cftsubsecpagefont}{\cftsubsecfont}
\renewcommand{\cftfigfont}{\sffamily}
\renewcommand{\cftfigpagefont}{\cftfigfont}
\renewcommand{\cfttabfont}{\sffamily}
\renewcommand{\cfttabpagefont}{\cfttabfont}
\renewcommand*{\l@lstlisting}[2]{\@dottedtocline{1}{1.5em}{2.3em}{\sffamily#1}{\sffamily#2}}
\let\Chapter\chapter
\def\chapter{\addtocontents{lol}{\protect\addvspace{10pt}}\Chapter}

% Header/footer style
\setlength{\headheight}{13.6pt}
\fancypagestyle{plain}{%
    \fancyhead{}
    \renewcommand{\headrulewidth}{0pt}
}

\fancyhf{}
\renewcommand{\headrulewidth}{0.3pt}
\renewcommand{\footrulewidth}{0.3pt}
\fancyhead[OL]{\sffamily\small\nouppercase\leftmark}
\fancyhead[ER]{\sffamily\small\nouppercase\rightmark}
\fancyfoot[OR,EL]{\sffamily\small\thepage}
\fancyfoot[OL,ER]{\sffamily\small{}Universität~Bielefeld,~AG~Technische~Informatik}

\pagestyle{fancy}
\renewcommand{\chaptermark}[1]{\markboth{\thechapter~#1}{}}
\renewcommand{\sectionmark}[1]{\markright{\thesection~#1}}

% No header on automatically inserted pages
\def\cleardoublepage{
    \clearpage
    \if@twoside
        \ifodd
            \c@page
        \else
            \null
%            \vspace*{\fill}
%            \begin{center}
%                This page intentionally contains only this sentence.
%            \end{center}
%            \vspace{\fill}
            \thispagestyle{plain}
            \newpage
            \if@twocolumn
                \mbox{}
                \newpage
            \fi
        \fi
    \fi
}

\def\cleardoublepageempty{
    \clearpage
    \if@twoside
        \ifodd
            \c@page
        \else
            \null
%            \vspace*{\fill}
%            \begin{center}
%                This page intentionally contains only this sentence.
%            \end{center}
%            \vspace{\fill}
            \thispagestyle{empty}
            \newpage
            \if@twocolumn
                \mbox{}
                \newpage
            \fi
        \fi
    \fi
}

% Paragraph settings
%\parindent0pt
%\parskip5pt

% Set footnote counter per whole document
\counterwithout{footnote}{chapter}
% Set space above footnotes
\setlength{\skip\footins}{\baselineskip}

% Listings default layout
\lstdefinelanguage{pseudo}{morekeywords={for,endfor,to,step,if,endif,function,endfunction,return,end}, sensitive=true}
\lstdefinelanguage[x64]{Assembler}[x86masm]{Assembler}{morekeywords={rsp,rax,r10,r10d,rcx,r11d,r10w,r11,r11w}, sensitive=false}
\lstdefinelanguage[SSE]{Assembler}[x64]{Assembler}{morekeywords={movaps,XMMWORD,xmm13,movdqa,movd,psrldq,xmm0,xmm1,movhlps,xmm5,movq,rcpps,mulps}, sensitive=false}
\lstdefinelanguage[types]{C++}[GNU]{C++}{morekeywords={uint16_t}, sensitive=true}
\lstdefinelanguage[SSE]{C++}[types]{C++}{morekeywords={_mm_abs_epi16,_mm_adds_epi16,_mm_subs_epi16,__m128,__m128i,_mm_store_si128,_mm_sad_epu8,SSEFloat,SSEByte,_mm_mul_ps,_mm_unpacklo_epi8,_mm_unpackhi_epi8,_mm_setzero_si128,SSEShort,_mm_cvtsi128_si32,_mm_srli_si128,_mm_add_epi32,SSEInt,_mm_hadd_epi32,_mm_shuffle_epi32,_mm_cvtepi32_ps,_mm_extract_epi32,_mm_srai_epi32,_mm_unpacklo_epi16,_mm_unpackhi_epi16,_mm_div_ps,_mm_packus_epi16,_mm_packs_epi32,_mm_cvtps_epi32,_mm_load_ps,_mm_add_ps,_mm_sub_ps,_mm_unpacklo_ps,_mm_unpackhi_ps,_mm_movelh_ps,_mm_movehl_ps,_mm_load_si128,_mm_extract_epi16, _mm_unpacklo_pd, _mm_unpackhi_pd}, sensitive=true}% min ?!
\lstdefinestyle{highlight}{keywordstyle=\bfseries}
\lstdefinestyle{nohighlight}{keywordstyle={}}
\lstdefinestyle{togglehighlight}{keywordstyle=\lst@ifdisplaystyle\bfseries\fi}
\lstdefinestyle{numbers}{numbers=right, numberstyle=\color{gray}\ttfamily, numbersep=-1em}
\lstdefinestyle{nonumbers}{numbers=none}
\lstdefinestyle{subfig}{abovecaptionskip=0pt, belowcaptionskip=0pt, aboveskip=0pt, belowskip=0pt}

\lstdefinestyle{tilisting}{language={[SSE]C++}, style=togglehighlight, basicstyle=\ttfamily\lst@ifdisplaystyle\scriptsize\fi, framerule=\heavyrulewidth, abovecaptionskip=\abovecaptionskip, belowcaptionskip=\belowcaptionskip, aboveskip=\intextsep, belowskip=\intextsep, frame=tb, captionpos=b, escapeinside={(*@}{@*)}}

\lstset{style=tilisting}

\newenvironment{lstlistingrow}{\begin{figure}\captionsetup{type=lstlisting}\lstset{belowskip=0pt, aboveskip=0pt}}{\end{figure}}

% Title setup
\titleformat{\chapter}[display]{\sffamily\fontsize{90}{11}\selectfont\bfseries}{\hfill\thechapter}{20pt}{\huge}[\titlerule]
\titleformat{\section}{\sffamily\Large\bfseries}{\thesection}{1em}{}
\titleformat{\subsection}{\sffamily\large\bfseries}{\thesubsection}{1em}{}
\titleformat{\subsubsection}{\sffamily\normalsize\bfseries}{\thesubsubsection}{1em}{}
\titleformat{\paragraph}[runin]{\sffamily\normalsize\bfseries}{\theparagraph}{1em}{}
\titleformat{\subparagraph}[runin]{\normalfont\normalsize\bfseries}{\thesubparagraph}{1em}{}
\titlespacing*{\chapter}{0pt}{50pt}{40pt}
\titlespacing*{\section}{0pt}{3.5ex plus 1ex minus .2ex}{2.3ex plus .2ex}
\titlespacing*{\subsection}{0pt}{3.25ex plus 1ex minus .2ex}{1.5ex plus .2ex}
\titlespacing*{\subsubsection}{0pt}{3.25ex plus 1ex minus .2ex}{1.5ex plus .2ex}
\titlespacing*{\paragraph}{0pt}{3.25ex plus 1ex minus .2ex}{1em}
\titlespacing*{\subparagraph}{\parindent}{3.25ex plus 1ex minus .2ex}{1em}

% Abstract
\renewenvironment{abstract}{%
    \newcounter{savepage}
    \setcounter{savepage}{\thepage}
    \titlepage
        \null\vfil
        \@beginparpenalty\@lowpenalty
        \begin{center}
            \sffamily\bfseries\abstractname
        \@endparpenalty\@M
        \end{center}}{%
    \par\vfil\null\endtitlepage
    \setcounter{page}{\thesavepage}
    \stepcounter{page}
}

%%%%%%%%%%%%%%%%%%%%%%%
% maketitle (02/2004, TK)
%           (08/2014, MP)

\def\tempempty{}
\gdef\betreuera#1{\def\tempin{#1}\ifx\tempin\tempempty \@betreuera \else \gdef\@betreuera{#1} \fi}
\gdef\@betreuera{\@latex@warning@no@line{No \noexpand\betreuera given}}
\gdef\betreuerb#1{\def\tempin{#1}\ifx\tempin\tempempty \@betreuerb \else \gdef\@betreuerb{#1} \fi}
\gdef\@betreuerb{\@latex@info@no@line{No \noexpand\betreuerb given}}
\gdef\arbeit#1{\def\tempin{#1}\ifx\tempin\tempempty \@arbeit \else \def\@arbeit{#1} \fi}
\gdef\@arbeit{\@latex@info@no@line{No \noexpand\arbeit given}}

\gdef\studiengang#1{\def\tempin{#1}\ifx\tempin\tempempty \@studiengang \else \gdef\@studiengang{#1} \fi}
\gdef\@studiengang{\@latex@warning@no@line{No \noexpand\studiengang given}}

\if@titlepage
    \renewcommand{\maketitle}{%
        \begin{titlepage}
            \enlargethispage{3cm}
            \headheight0pt
            \headsep0pt
            \topmargin0pt
            \footskip0pt
            \sffamily
            \begin{center}
                Universität Bielefeld\\
                Technische Fakultät\par
                \vspace*{4.5cm}
                {\Large\@arbeit}\\
                \vspace*{0.5cm}
                {\LARGE\@title\par}
                \vspace*{1cm}
                {\large{}im Studiengang\\
                \@studiengang\par}
                \vspace*{2.5cm}
                {\Large\@author}\par
                \vspace*{0.75cm}
                {\large14. Juli 2022}\par%\@date}\par
                \vfill
                \textbf{Betreuer}\\
                \@betreuera\\
                \@betreuerb
            \end{center}\par
            \@thanks
        \end{titlepage}
        \cleardoublepageempty
        \setcounter{footnote}{0}
        \global\let\thanks\relax
        \global\let\maketitle\relax
        \global\let\@thanks\@empty
        \global\let\@author\@empty
        \global\let\@date\@empty
        \global\let\@title\@empty
        %\global\let\@betreuera\@empty
        %\global\let\@betreuerb\@empty
        %\global\let\@gutachtera\@empty
        %\global\let\@gutachterb\@empty
        %\global\let\gutachtera\relax
        %\global\let\gutachterb\relax
        %\global\let\betreuera\relax
        %\global\let\betreuerb\relax
        \global\let\title\relax
        \global\let\author\relax
        \global\let\date\relax
        \global\let\and\relax
    }
\fi

% End of maketitle
%%%%%%%%%%%%%%%%%%%%%%%

%%%%%%%%%%%%%%%%%%%%%%%
% Statement (02/2004, TK; 09/2006, TK)
%           (08/2014, MP)

\newcommand{\erklaerung}[2]{%
    \thispagestyle{empty}
    \vspace*{\fill} % since \vfill does not work

    %\ifx\@englishtrue\undefined    % = ngerman
    \noindent{}Hiermit versichere ich, dass ich diese \@arbeit{} selbständig bearbeitet habe. Ich habe keine anderen als die angegebenen Quellen und Hilfsmittel benutzt und entsprechende Zitate kenntlich gemacht.\\[\baselineskip]
    Bielefeld, den #2
    %
    %\else   % = enlish
    %\fi
    %
    \vspace*{2cm}
    \begin{center}
    #1
    \end{center}
    \cleardoublepageempty
}
%
%%%%%%%%%%%%%%%%%%%%%%%
