
PROJECT(LatexProject NONE)
CMAKE_MINIMUM_REQUIRED(VERSION 3.11)

FIND_PACKAGE(LATEX)
IF(LATEX_FOUND)
	IF(LUALATEX__FOUND)
		MESSAGE(STATUS "lualatex found - ${LUALATEX_COMPILER}")
	ENDIF()
	IF(BIBTEX_FOUND)
		MESSAGE(STATUS "bibtex found - ${BIBTEX_COMPILER}")
	ENDIF()
	IF(MAKEINDEX_FOUND)
		MESSAGE(STATUS "makeindex found - ${MAKEINDEX_COMPILER}")
	ENDIF()
    IF(PDFLATEX_FOUND)
    	MESSAGE(STATUS "pdflatex found - ${PDFLATEX_COMPILER}")
	ENDIF()
ELSE()
	MESSAGE(ERROR "No latex tools found!")
ENDIF()


# The directory where the final build data will be stored.
SET(LATEX_OUTPUT_PATH build)
SET(OUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${LATEX_OUTPUT_PATH}")
# The source file containing the reference for cites.
SET(BIB_REFERENCE_PATH ${CMAKE_SOURCE_DIR}/references.bib)

# Latex source file.
SET( MAIN_TEX_BASE_FILENAME "aadg1/main")
SET( MAIN_TEX "${CMAKE_SOURCE_DIR}/src/${MAIN_TEX_BASE_FILENAME}.tex")
SET( MAIN_IDX "${LATEX_OUTPUT_PATH}/${MAIN_TEX_BASE_FILENAME}.idx")
SET( MAIN_AUX "${LATEX_OUTPUT_PATH}/${MAIN_TEX_BASE_FILENAME}.aux")

# Directory where the source files are located.
SET( WORKINGDIR "${CMAKE_SOURCE_DIR}/src" )

SET( PDFLATEX_ARGS "--shell-escape" "--enable-pipes" "--enable-write18" )

# First pass.
ADD_CUSTOM_TARGET( latex-prebuild
		COMMAND ${PDFLATEX_COMPILER}  ${PDFLATEX_ARGS} -output-directory ${OUT_DIRECTORY} -draftmode -interaction=nonstopmode ${MAIN_TEX}
		COMMAND ${PDFLATEX_COMPILER}  ${PDFLATEX_ARGS} -output-directory ${OUT_DIRECTORY} -draftmode -interaction=nonstopmode ${MAIN_TEX}
		COMMENT "Starting Prebuild."
		WORKING_DIRECTORY ${WORKINGDIR}
		DEPENDS ${MAIN_TEX})

# Generate the indices for the index table.
ADD_CUSTOM_TARGET( latex-makeindex
			COMMAND ${MAKEINDEX_COMPILER} ${MAIN_IDX}
			COMMENT "Read and create indices with ${MAIN_IDX}."
			DEPENDS ${MAIN_IDX})
ADD_DEPENDENCIES(latex-makeindex latex-prebuild)

# Generate what citation found in the latex file.
ADD_CUSTOM_TARGET( latex-bibreferences
			COMMAND ${BIBTEX_COMPILER} build/main.aux
			COMMENT "Read and create main bib references file."
			DEPENDS  ${CMAKE_SOURCE_DIR}/build/main.aux)
ADD_DEPENDENCIES( latex-bibreferences latex-prebuild)

# Second pass - generate the final pdf.
ADD_CUSTOM_TARGET( latex-pdf	
			COMMAND ${PDFLATEX_COMPILER} ${PDFLATEX_ARGS} -output-directory ${OUT_DIRECTORY} ${MAIN_TEX} 
			WORKING_DIRECTORY ${WORKINGDIR}
			COMMENT "Assembling the final pdf file."
			DEPENDS  ${MAIN_TEX})
ADD_DEPENDENCIES( latex-pdf latex-prebuild latex-makeindex latex-bibreferences)

ADD_CUSTOM_TARGET(all-formats ALL) # Entry point of execution.
ADD_DEPENDENCIES( all-formats latex-pdf)