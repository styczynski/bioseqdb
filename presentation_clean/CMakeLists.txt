
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
	IF(BIBER_FOUND)
		MESSAGE(STATUS "biber found - ${BIBER_COMPILER}")
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
SET(LATEX_ARTIFACTS_PATH_PDFS pdf)
# The source file containing the reference for cites.
# SET(BIB_REFERENCE_PATH ${CMAKE_SOURCE_DIR}/references.bib)

# Latex source file.
# SET( MAIN_TEX_BASE_FILENAME "main")
# SET( MAIN_TEX "${CMAKE_SOURCE_DIR}/src/aadg1/${MAIN_TEX_BASE_FILENAME}.tex")
# SET( MAIN_IDX "${LATEX_OUTPUT_PATH}/${MAIN_TEX_BASE_FILENAME}.idx")
# SET( MAIN_AUX "${LATEX_OUTPUT_PATH}/${MAIN_TEX_BASE_FILENAME}.aux")

# Directory where the source files are located.
SET( WORKINGDIR "${CMAKE_SOURCE_DIR}/src" )

SET( PDFLATEX_ARGS "--shell-escape" "--enable-pipes" "--enable-write18" )
SET( OUT_DIRECTORY_PDF "${CMAKE_SOURCE_DIR}/${LATEX_ARTIFACTS_PATH_PDFS}" )


set(LATEX_ALL_TARGETS "")

FILE(GLOB TEX_MAIN_SOURCES "${CMAKE_SOURCE_DIR}/src/**/main.tex")
FOREACH(MAIN_TEX ${TEX_MAIN_SOURCES})

	GET_FILENAME_COMPONENT(TEX_JOB_DIR ${MAIN_TEX} DIRECTORY)
	GET_FILENAME_COMPONENT(TEX_JOB ${TEX_JOB_DIR} NAME)
	#GET_FILENAME_COMPONENT(MAIN_TEX_BASE_FILENAME ${MAIN_TEX} NAME_WE)
	SET( JOB_OUT_DIRECTORY "${CMAKE_SOURCE_DIR}/${LATEX_OUTPUT_PATH}/${TEX_JOB}" )

	ADD_CUSTOM_COMMAND(
		OUTPUT "${JOB_OUT_DIRECTORY}/main.tex"
		COMMAND mkdir -p ${JOB_OUT_DIRECTORY}
		COMMAND mkdir -p ${JOB_OUT_DIRECTORY}/robustExternalize
		COMMAND cp -rf ${CMAKE_SOURCE_DIR}/src/aadg1/* ${JOB_OUT_DIRECTORY}
		COMMAND cp ${CMAKE_SOURCE_DIR}/src/common.tex ${JOB_OUT_DIRECTORY}
		COMMAND ${BIBER_COMPILER} --tool --validate-datamodel ${JOB_OUT_DIRECTORY}/main.bib
		WORKING_DIRECTORY ${WORKINGDIR}
		MAIN_DEPENDENCY ${MAIN_TEX})

	ADD_CUSTOM_COMMAND(
			OUTPUT "${JOB_OUT_DIRECTORY}/main.aux"
			COMMAND python3 ${CMAKE_SOURCE_DIR}/configure.py ${JOB_OUT_DIRECTORY}/vars.tex "draft=true"
			COMMAND ${PDFLATEX_COMPILER}  ${PDFLATEX_ARGS} -output-directory ${JOB_OUT_DIRECTORY} -draftmode -interaction nonstopmode -halt-on-error -file-line-error ${JOB_OUT_DIRECTORY}/main.tex
			COMMENT "Starting Prebuild."
			WORKING_DIRECTORY ${JOB_OUT_DIRECTORY}
			DEPENDS ${JOB_OUT_DIRECTORY}/main.tex)

	# Generate the indices for the index table.
	# ADD_CUSTOM_TARGET( latex-makeindex
	# 			COMMAND ${MAKEINDEX_COMPILER} ${MAIN_IDX}
	# 			COMMENT "Read and create indices with ${MAIN_IDX}."
	# 			DEPENDS ${MAIN_IDX})
	# ADD_DEPENDENCIES(latex-makeindex latex-prebuild)

	# Generate what citation found in the latex file.
	ADD_CUSTOM_COMMAND(
				OUTPUT "${JOB_OUT_DIRECTORY}/main.bbl" "${JOB_OUT_DIRECTORY}/main.bbg"
				COMMAND ${BIBER_COMPILER} main
				COMMENT "Read and create main bib references file."
				WORKING_DIRECTORY ${JOB_OUT_DIRECTORY}
				DEPENDS  ${JOB_OUT_DIRECTORY}/main.aux)
	#ADD_DEPENDENCIES( "latex-bibreferences-${TEX_JOB}" "latex-prebuild-${TEX_JOB}")

	# Second pass - generate the final pdf.
	# ADD_CUSTOM_TARGET("latex-${TEX_JOB}"
	# 			COMMAND python3 ${CMAKE_SOURCE_DIR}/configure.py ${JOB_OUT_DIRECTORY}/vars.tex "draft=false"
	# 			COMMAND ${PDFLATEX_COMPILER} ${PDFLATEX_ARGS} -output-directory ${JOB_OUT_DIRECTORY} ${JOB_OUT_DIRECTORY}/main.tex
	# 			COMMAND mkdir -p ${OUT_DIRECTORY_PDF}
	# 			COMMAND cp -rf ${JOB_OUT_DIRECTORY}/main.pdf ${OUT_DIRECTORY_PDF}/${TEX_JOB}.pdf
	# 			WORKING_DIRECTORY ${JOB_OUT_DIRECTORY}
	# 			COMMENT "Assembling the final pdf file."
	# 			DEPENDS  ${JOB_OUT_DIRECTORY}/main.tex)
	# ADD_DEPENDENCIES("latex-${TEX_JOB}" "latex-prebuild-${TEX_JOB}" "latex-bibreferences-${TEX_JOB}")
	# LIST(APPEND LATEX_ALL_TARGETS "latex-${TEX_JOB}")

	ADD_CUSTOM_COMMAND(
		OUTPUT ${OUT_DIRECTORY_PDF}/${TEX_JOB}.pdf
		COMMAND python3 ${CMAKE_SOURCE_DIR}/configure.py ${JOB_OUT_DIRECTORY}/vars.tex "draft=false"
		COMMAND ${PDFLATEX_COMPILER} ${PDFLATEX_ARGS} -output-directory ${JOB_OUT_DIRECTORY} ${JOB_OUT_DIRECTORY}/main.tex
		COMMAND mkdir -p ${OUT_DIRECTORY_PDF}
		COMMAND cp -rf ${JOB_OUT_DIRECTORY}/main.pdf ${OUT_DIRECTORY_PDF}/${TEX_JOB}.pdf
		WORKING_DIRECTORY ${JOB_OUT_DIRECTORY}
		COMMENT "Assembling the final pdf file."
		DEPENDS  ${JOB_OUT_DIRECTORY}/main.tex "${JOB_OUT_DIRECTORY}/main.bbl" "${JOB_OUT_DIRECTORY}/main.bbg")
	LIST(APPEND LATEX_ALL_TARGETS "${OUT_DIRECTORY_PDF}/${TEX_JOB}.pdf")

ENDFOREACH()

ADD_CUSTOM_TARGET("latex-all"
	DEPENDS ${LATEX_ALL_TARGETS}
)

ADD_CUSTOM_TARGET(all-formats ALL) # Entry point of execution.
ADD_DEPENDENCIES( all-formats "latex-all")