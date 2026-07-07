################################################################################
#
# python-sgp4
#
################################################################################

PYTHON_SGP4_VERSION = 2.24
PYTHON_SGP4_SOURCE = sgp4-$(PYTHON_SGP4_VERSION).tar.gz
PYTHON_SGP4_SITE = https://files.pythonhosted.org/packages/10/fa/4251a1cac8a23ccc552ed367ae7fa7439f631022b7db5472ce99f43b49b3
PYTHON_SGP4_LICENSE = MIT
PYTHON_SGP4_LICENSE_FILES = LICENSE
PYTHON_SGP4_SETUP_TYPE = distutils

$(eval $(python-package))
